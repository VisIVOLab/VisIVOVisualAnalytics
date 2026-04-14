#ifndef OIDCAuthorizationCodeFlow_H
#define OIDCAuthorizationCodeFlow_H

#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkRequest;
class QOAuth2AuthorizationCodeFlow;
class QTimer;
class VisIVOUrlSchemeHandler;

class OIDCAuthorizationCodeFlow : public QObject
{
    Q_OBJECT

public:
    explicit OIDCAuthorizationCodeFlow(const QString &authUrl, const QString &tokenUrl,
                                       const QString &scope, const QString &clientId,
                                       const QString &clientSecret, QObject *parent = nullptr);

    bool hasTokens() const;
    void putAuthorizationHeader(QNetworkRequest &req);
    /// Returns the raw bearer token, or an empty string if not authenticated.
    QString getAccessToken() const { return this->accessToken; }

public slots:
    void grant();
    void logout();

signals:
    void granted();
    void loggedOut();

private slots:
    void authorizeWithWebView(const QUrl &url);
    void authorizationCallbackReceived(const QVariantMap &data);
    void tokensReceived(const QByteArray &data);
    void refreshTokens();

private:
    QPointer<QNetworkAccessManager> nam;
    QPointer<QOAuth2AuthorizationCodeFlow> auth;
    QPointer<VisIVOUrlSchemeHandler> urlSchemeHandler;
    QPointer<QTimer> refreshTimer;

    QString accessToken;
    QString refreshToken;
};

#endif
