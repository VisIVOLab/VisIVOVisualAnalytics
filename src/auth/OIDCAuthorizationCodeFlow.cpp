#include "OIDCAuthorizationCodeFlow.h"

#include "HttpServerReplyHandler.h"
#include "SecureStore.h"
#include "VisIVOUrlSchemeHandler.h"

#include <QApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOAuth2AuthorizationCodeFlow>
#include <QThreadPool>
#include <QTimer>
#include <QUrlQuery>
#include <QWebEngineProfile>
#include <QWebEngineView>

using namespace Qt::StringLiterals;

OIDCAuthorizationCodeFlow::OIDCAuthorizationCodeFlow(const QString &authUrl,
                                                     const QString &tokenUrl, const QString &scope,
                                                     const QString &clientId,
                                                     const QString &clientSecret, QObject *parent)
    : QObject(parent)
{
    QSet<QByteArray> scopes;
    for (const auto &s : scope.split(' ')) {
        scopes.insert(s.toUtf8());
    }

    this->nam = new QNetworkAccessManager(this);
    this->auth = new QOAuth2AuthorizationCodeFlow(this->nam, this);
    this->auth->setReplyHandler(new HttpServerReplyHandler(this));
    this->auth->setAuthorizationUrl(QUrl(authUrl));
    this->auth->setTokenUrl(QUrl(tokenUrl));
    this->auth->setRequestedScopeTokens(scopes);
    this->auth->setClientIdentifier(clientId);
    this->auth->setClientIdentifierSharedKey(clientSecret);
    QObject::connect(this->auth, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, this,
                     &OIDCAuthorizationCodeFlow::authorizeWithWebView);

    this->urlSchemeHandler = new VisIVOUrlSchemeHandler(this);
    QObject::connect(this->urlSchemeHandler, &VisIVOUrlSchemeHandler::callbackReceived, this,
                     &OIDCAuthorizationCodeFlow::authorizationCallbackReceived);

    this->refreshTimer = new QTimer(this);
    QObject::connect(this->refreshTimer, &QTimer::timeout, this,
                     &OIDCAuthorizationCodeFlow::refreshTokens);

    // Pick up the sign-in of an earlier session, once the event loop runs.
    QTimer::singleShot(0, this, &OIDCAuthorizationCodeFlow::restoreSession);
}

namespace {
const QString kStoreService = u"VisIVO Visual Analytics"_s;

/// One thread for every secure-store call: they run in the order they were
/// made (a write is never overtaken by an earlier removal), and a store that
/// blocks — libsecret waiting for a keyring to be unlocked — never blocks the
/// interface.
QThreadPool *storePool()
{
    // Deliberately never destroyed: a pool owned by the application would be
    // joined at exit, and a store call blocked on a locked keyring would then
    // keep the application from quitting.
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(1);
        // …but give pending calls a bounded chance to finish on quit, so a
        // sign-out just before closing is not lost and the next start does not
        // sign back in. Two seconds covers any store that is not stuck.
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp,
                         [p]() { p->waitForDone(2000); });
        return p;
    }();
    return pool;
}

/// Tell the user, on the GUI thread, that the secure store refused an
/// operation. The secret itself is never part of the message.
void reportStoreFailure(const QString &what)
{
    QMetaObject::invokeMethod(
            qApp,
            [what]() {
                QMessageBox::warning(nullptr, u"Sign-in storage"_s,
                                     u"%1 The system's secure store (Keychain, Credential "
                                     u"Manager or Secret Service) refused the operation."_s
                                             .arg(what));
            },
            Qt::QueuedConnection);
}
}   // namespace

QString OIDCAuthorizationCodeFlow::storeAccount() const
{
    return u"oidc-refresh-token|%1|%2"_s.arg(this->auth->tokenUrl().toString(),
                                             this->auth->clientIdentifier());
}

void OIDCAuthorizationCodeFlow::forgetStoredSession()
{
    if (!visivo::secure::available())
        return;
    const QString account = this->storeAccount();
    storePool()->start([account]() {
        if (!visivo::secure::remove(kStoreService, account))
            reportStoreFailure(u"The stored VLKB sign-in could not be removed, so the next "
                               u"start may sign you in again."_s);
    });
}

void OIDCAuthorizationCodeFlow::storeSession()
{
    if (!visivo::secure::available() || this->refreshToken.isEmpty())
        return;
    const QString account = this->storeAccount();
    const QString token = this->refreshToken;
    storePool()->start([account, token]() {
        if (!visivo::secure::write(kStoreService, account, token))
            reportStoreFailure(u"The VLKB sign-in could not be kept for the next start; you "
                               u"will be asked to sign in again then."_s);
    });
}

void OIDCAuthorizationCodeFlow::restoreSession()
{
    if (!this->isConfigured() || this->hasTokens() || !visivo::secure::available())
        return;
    // Read off the GUI thread; continue only if nothing has happened meanwhile
    // (the user signed in or out while the store was being read).
    const QString account = this->storeAccount();
    const int at = this->session;
    QPointer<OIDCAuthorizationCodeFlow> self(this);
    storePool()->start([self, account, at]() {
        const QString stored = visivo::secure::read(kStoreService, account);
        QMetaObject::invokeMethod(
                qApp,
                [self, stored, at]() {
                    if (!self || stored.isEmpty() || self->session != at || self->hasTokens())
                        return;
                    self->refreshToken = stored;
                    self->restoring = true;
                    self->refreshTokens();
                },
                Qt::QueuedConnection);
    });
}

bool OIDCAuthorizationCodeFlow::hasTokens() const
{
    return !this->accessToken.isEmpty();
}

bool OIDCAuthorizationCodeFlow::isConfigured() const
{
    return this->auth->authorizationUrl().isValid() && !this->auth->authorizationUrl().isEmpty()
           && this->auth->tokenUrl().isValid() && !this->auth->tokenUrl().isEmpty()
           && !this->auth->clientIdentifier().isEmpty();
}

void OIDCAuthorizationCodeFlow::putAuthorizationHeader(QNetworkRequest &req)
{
    if (this->hasTokens()) {
        const QByteArray value = "Bearer "_ba.append(this->accessToken.toUtf8());
        req.setRawHeader("Authorization"_ba, value);
    }
}

void OIDCAuthorizationCodeFlow::grant()
{
    if (!this->isConfigured()) {
        qWarning() << "OIDC sign-in requested but this build has no endpoints/client id configured.";
        return;
    }
    this->auth->grant();
}

void OIDCAuthorizationCodeFlow::logout()
{
    ++this->session;
    this->restoring = false;
    this->forgetStoredSession();
    this->refreshTimer->stop();
    this->accessToken.clear();
    this->refreshToken.clear();
    emit this->loggedOut();
}

void OIDCAuthorizationCodeFlow::authorizeWithWebView(const QUrl &url)
{
    auto profile = new QWebEngineProfile;
    profile->installUrlSchemeHandler("vlva"_ba, this->urlSchemeHandler);

    // The sign-in is started from the Settings dialog, which is modal: a
    // parentless window opened under it is blocked by that modality, stacks
    // behind it and can never take focus. Parent the login page to whatever
    // modal window is open and make it modal itself, so it opens on top and is
    // the window that receives input until the user signs in or closes it.
    QWidget *modal = QApplication::activeModalWidget();
    auto view = new QWebEngineView(profile, modal);
    view->setWindowFlag(Qt::Window);
    // Application-modal, not window-modal: on macOS a window-modal child becomes
    // a sheet glued to the dialog, too small for a login page.
    view->setWindowModality(modal ? Qt::ApplicationModal : Qt::NonModal);
    view->setWindowTitle(u"Sign in to the VLKB"_s);
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->setContextMenuPolicy(Qt::NoContextMenu);
    view->resize(1024, 768);
    view->load(url);
    view->show();
    view->raise();
    view->activateWindow();

    QObject::connect(this->urlSchemeHandler, &VisIVOUrlSchemeHandler::callbackReceived, view,
                     &QWebEngineView::close);
    QObject::connect(view, &QWebEngineView::destroyed, profile, &QWebEngineProfile::deleteLater);
}

void OIDCAuthorizationCodeFlow::authorizationCallbackReceived(const QVariantMap &data)
{
    QByteArray authHeader =
            u"%1:%2"_s.arg(this->auth->clientIdentifier(), this->auth->clientIdentifierSharedKey())
                    .toUtf8()
                    .toBase64();
    authHeader.prepend("Basic "_ba);

    QNetworkRequest req(this->auth->tokenUrl());
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    req.setRawHeader("Authorization"_ba, authHeader);

    QUrlQuery postData;
    postData.addQueryItem(u"grant_type"_s, u"authorization_code"_s);
    postData.addQueryItem(u"client_id"_s, this->auth->clientIdentifier());
    postData.addQueryItem(u"client_secret"_s, this->auth->clientIdentifierSharedKey());
    postData.addQueryItem(u"redirect_uri"_s, this->auth->replyHandler()->callback());
    postData.addQueryItem(u"code"_s, data.value(u"code"_s).toString());
    const QByteArray postDataEncoded = postData.toString(QUrl::FullyEncoded).toUtf8();

    // A new sign-in supersedes anything still in flight (a startup restore,
    // a refresh of the previous session).
    const int at = ++this->session;
    this->restoring = false;
    auto reply = this->nam->post(req, postDataEncoded);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, at]() {
        reply->deleteLater();
        if (at != this->session)
            return;   // signed out, or signed in again, meanwhile

        if (reply->error()) {
            qCritical() << "Could not get user's tokens:" << reply->errorString();
            QMessageBox::critical(nullptr, u"Error"_s, reply->errorString());
            return;
        }

        this->tokensReceived(reply->readAll(), /*initialSignIn=*/true);
        emit this->granted();
    });
}

void OIDCAuthorizationCodeFlow::tokensReceived(const QByteArray &data, bool initialSignIn)
{
    const QJsonObject response = QJsonDocument::fromJson(data).object();
    this->accessToken = response.value("access_token"_L1).toString();
    // A refresh answer may or may not rotate the refresh token: keep the one we
    // have unless a new one came back, or the next refresh has nothing to use.
    // A new sign-in is a new session, possibly another user's: it never
    // inherits the previous refresh token.
    const QString rotated = response.value("refresh_token"_L1).toString();
    if (initialSignIn || !rotated.isEmpty())
        this->refreshToken = rotated;
    if (initialSignIn && rotated.isEmpty())
        this->forgetStoredSession();
    // Kept in the platform's secure store (never in the settings file), so the
    // next start can sign in again without the login page.
    this->storeSession();

    const int expiration = response.value("expires_in"_L1).toInt();
    if (expiration > 60) {
        this->refreshTimer->start((expiration - 60) * 1000);
    }
}

void OIDCAuthorizationCodeFlow::refreshTokens()
{
    if (this->refreshToken.isEmpty()) {
        qWarning() << "Trying to refresh user's tokens but the user is no longer "
                      "logged in. Stopping the timer...";
        this->refreshTimer->stop();
        return;
    }

    QByteArray authHeader =
            u"%1:%2"_s.arg(this->auth->clientIdentifier(), this->auth->clientIdentifierSharedKey())
                    .toUtf8()
                    .toBase64();
    authHeader.prepend("Basic "_ba);

    QNetworkRequest req(this->auth->tokenUrl());
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    req.setRawHeader("Authorization"_ba, authHeader);

    QUrlQuery postData;
    postData.addQueryItem(u"grant_type"_s, u"refresh_token"_s);
    postData.addQueryItem(u"client_id"_s, this->auth->clientIdentifier());
    postData.addQueryItem(u"client_secret"_s, this->auth->clientIdentifierSharedKey());
    postData.addQueryItem(u"refresh_token"_s, this->refreshToken);
    const QByteArray postDataEncoded = postData.toString(QUrl::FullyEncoded).toUtf8();

    // Whether this is the silent startup refresh is decided now, per request,
    // not when the answer comes back.
    const bool silent = this->restoring;
    this->restoring = false;
    const int at = this->session;
    auto reply = this->nam->post(req, postDataEncoded);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, at, silent]() {
        reply->deleteLater();
        if (at != this->session)
            return;   // signed out, or signed in again, meanwhile: not ours to apply

        if (reply->error()) {
            qCritical() << "Could not refresh user's tokens:" << reply->errorString();
            const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool refused = status == 400 || status == 401;
            if (silent && !refused) {
                // The SSO could not be reached at startup (no network, a
                // timeout): the stored token may still be good. Start signed
                // out, keep it for the next start, and say nothing.
                this->refreshTimer->stop();
                this->accessToken.clear();
                this->refreshToken.clear();
                return;
            }
            // The token was refused (expired, revoked): forget it everywhere.
            this->logout();
            if (!silent)
                QMessageBox::critical(nullptr, u"Error"_s, reply->errorString());
            return;
        }

        this->tokensReceived(reply->readAll());
        if (silent && this->hasTokens())
            emit this->granted();
    });
}
