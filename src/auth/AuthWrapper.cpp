#include "AuthWrapper.h"

#include "AuthKeys.h"
#include "OIDCAuthorizationCodeFlow.h"
#include "app/DiagnosticsManager.h"

static const char *serviceName(AuthService s)
{
    return (s == AuthService::VLKB) ? "VLKB" : "unknown";
}

AuthWrapper::AuthWrapper(QObject *parent) : QObject(parent)
{
    this->vlkb = new OIDCAuthorizationCodeFlow(VLKB_AUTH_URL, VLKB_TOKEN_URL, VLKB_SCOPE,
                                               VLKB_CLIENT_ID, VLKB_SECRET, this);
    QObject::connect(this->vlkb, &OIDCAuthorizationCodeFlow::granted, this,
                     [this]() {
                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                 QStringLiteral("AuthWrapper/OIDC"),
                                 QStringLiteral("VLKB: OIDC token granted (access token issued).") );
                         emit this->granted(AuthService::VLKB);
                     });
    QObject::connect(this->vlkb, &OIDCAuthorizationCodeFlow::loggedOut, this,
                     [this]() {
                         DiagnosticsManager::instance()->publish(
                                 DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
                                 QStringLiteral("AuthWrapper/OIDC"),
                                 QStringLiteral("VLKB: user logged out (tokens cleared).") );
                         emit this->loggedOut(AuthService::VLKB);
                     });
}

bool AuthWrapper::hasTokens(AuthService service) const
{
    return this->authObject(service)->hasTokens();
}

bool AuthWrapper::isConfigured(AuthService service) const
{
    const auto *obj = this->authObject(service);
    return obj && obj->isConfigured();
}

void AuthWrapper::putAuthorizationHeader(AuthService service, QNetworkRequest &req)
{
    this->authObject(service)->putAuthorizationHeader(req);
}

QString AuthWrapper::bearerToken(AuthService service) const
{
    auto *obj = this->authObject(service);
    return obj ? obj->getAccessToken() : QString();
}

void AuthWrapper::grant(AuthService service)
{
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
            QStringLiteral("AuthWrapper/OIDC"),
            QStringLiteral("%1: OIDC grant requested (opening browser-based PKCE flow).")
                    .arg(QString::fromLatin1(serviceName(service))));
    this->authObject(service)->grant();
}

void AuthWrapper::logout(AuthService service)
{
    DiagnosticsManager::instance()->publish(
            DiagnosticsLevel::Info, DiagnosticsCategory::Remote,
            QStringLiteral("AuthWrapper/OIDC"),
            QStringLiteral("%1: logout requested.")
                    .arg(QString::fromLatin1(serviceName(service))));
    this->authObject(service)->logout();
}

OIDCAuthorizationCodeFlow *AuthWrapper::authObject(AuthService service) const
{
    return (service == AuthService::VLKB) ? this->vlkb : nullptr;
}
