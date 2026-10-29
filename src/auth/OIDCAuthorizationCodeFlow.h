#ifndef OIDCAuthorizationCodeFlow_H
#define OIDCAuthorizationCodeFlow_H

#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkRequest;
class QOAuth2AuthorizationCodeFlow;
class QTimer;
class QUrlQuery;
class VisIVOUrlSchemeHandler;

class OIDCAuthorizationCodeFlow : public QObject
{
    Q_OBJECT

public:
    explicit OIDCAuthorizationCodeFlow(const QString &authUrl, const QString &tokenUrl,
                                       const QString &scope, const QString &clientId,
                                       const QString &clientSecret, QObject *parent = nullptr);

    bool hasTokens() const;
    /// True when the build carries the endpoints and client id the flow needs.
    /// Without them grant() has nowhere to send the user and does nothing.
    bool isConfigured() const;
    /// What the user is signing in to ("the VLKB", "SKAVA"): the login
    /// window's title and the messages name it.
    void setDisplayName(const QString &name) { this->displayName = name; }
    QString getDisplayName() const { return this->displayName; }
    void putAuthorizationHeader(QNetworkRequest &req);
    /// Returns the raw bearer token, or an empty string if not authenticated.
    QString getAccessToken() const { return this->accessToken; }

public slots:
    void grant();
    void logout();

signals:
    /// A sign-in completed (the user's, or the silent one at startup).
    void granted();
    /// A new access token is in hand: after a sign-in and after every
    /// refresh. Consumers that hold the token itself follow this one.
    void accessTokenChanged();
    void loggedOut();

private slots:
    void authorizeWithWebView(const QUrl &url);
    void authorizationCallbackReceived(const QVariantMap &data);
    void tokensReceived(const QByteArray &data, bool initialSignIn = false);
    void refreshTokens();
    /// At startup: if the secure store holds a refresh token from an earlier
    /// session, exchange it for fresh tokens without showing the login.
    void restoreSession();

private:
    /// The secure-store account this client's refresh token is kept under:
    /// the token endpoint and the client id, so a different SSO or client
    /// never picks up another's token.
    QString storeAccount() const;
    void forgetStoredSession();
    /// Keep the current refresh token in the secure store (off the GUI thread).
    void storeSession();

    QPointer<QNetworkAccessManager> nam;
    QPointer<QOAuth2AuthorizationCodeFlow> auth;
    QPointer<VisIVOUrlSchemeHandler> urlSchemeHandler;
    QPointer<QTimer> refreshTimer;

    /// The token request's client authentication: HTTP Basic with the
    /// secret for a confidential client; a public client (no secret) sends
    /// only its client_id in the body.
    void putClientAuthentication(QNetworkRequest &req, QUrlQuery &postData) const;

    QString accessToken;
    QString refreshToken;
    QString displayName{ QStringLiteral("the VLKB") };
    bool restoring{ false };   ///< the next refresh is the silent startup one
    /// Bumped by every logout and every new sign-in. A token reply carries the
    /// value it was requested under and is dropped if it no longer matches, so
    /// a late answer can neither undo a logout nor overwrite a newer session.
    int session{ 0 };
};

#endif
