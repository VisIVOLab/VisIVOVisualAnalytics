#include "VlkbQueryService.h"
#include "auth/AuthWrapper.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

using namespace Qt::StringLiterals;

VlkbQueryService::VlkbQueryService(const QString &baseUrl, AuthWrapper *auth, QObject *parent)
    : QObject(parent), m_baseUrl(baseUrl), m_auth(auth)
{
}

// ── Public API ────────────────────────────────────────────────────────────────

void VlkbQueryService::searchCircle(double l, double b, double r)
{
    if (l < 0.)
        l += 180.;

    QUrlQuery q;
    q.addQueryItem(u"POS"_s,
                   u"CIRCLE %1 %2 %3"_s.arg(QString::number(l),
                                             QString::number(b),
                                             QString::number(r)));
    q.addQueryItem(u"POSSYS"_s, u"GALACTIC"_s);

    QUrl url(m_baseUrl + u"/siav2/query"_s);
    url.setQuery(q);

    doGet(url,
          [this](const QByteArray &data) { emit searchFinished(data); },
          [this](const QString &err)     { emit searchFailed(err); });
}

void VlkbQueryService::searchRange(double l, double b, double dl, double db)
{
    double l1 = l - dl;
    double l2 = l + dl;
    if (l1 < 0.) l1 += 180.;
    if (l2 < 0.) l2 += 180.;

    QUrlQuery q;
    q.addQueryItem(u"POS"_s,
                   u"RANGE %1 %2 %3 %4"_s.arg(QString::number(l1),
                                               QString::number(l2),
                                               QString::number(b - db),
                                               QString::number(b + db)));
    q.addQueryItem(u"POSSYS"_s, u"GALACTIC"_s);

    QUrl url(m_baseUrl + u"/siav2/query"_s);
    url.setQuery(q);

    doGet(url,
          [this](const QByteArray &data) { emit searchFinished(data); },
          [this](const QString &err)     { emit searchFailed(err); });
}

void VlkbQueryService::downloadCutout(const QString &ivoID, const QString &pos,
                                       const QDir &destDir)
{
    QUrlQuery q;
    q.addQueryItem(u"ID"_s,     ivoID);
    q.addQueryItem(u"POS"_s,    pos);
    q.addQueryItem(u"POSSYS"_s, u"GALACTIC"_s);

    QUrl url(m_baseUrl + u"/soda/sync"_s);
    url.setQuery(q);

    const QString destPath =
            destDir.absoluteFilePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + u".fits"_s);

    doGet(url,
          [this, destPath](const QByteArray &data) {
              QFile f(destPath);
              if (!f.open(QIODevice::WriteOnly)) {
                  emit cutoutFailed(
                          u"Cannot write cutout to \"%1\": %2"_s.arg(destPath, f.errorString()));
                  return;
              }
              f.write(data);
              f.close();
              emit cutoutFinished(destPath);
          },
          [this](const QString &err) { emit cutoutFailed(err); });
}

// ── Private ───────────────────────────────────────────────────────────────────

void VlkbQueryService::doGet(const QUrl &url,
                              std::function<void(const QByteArray &)> onOk,
                              std::function<void(const QString &)>    onErr)
{
    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(url);

    if (m_auth && m_auth->hasTokens(AuthService::VLKB)) {
        m_auth->putAuthorizationHeader(AuthService::VLKB, req);
    }

    auto *reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [reply, nam, onOk = std::move(onOk), onErr = std::move(onErr)]() {
                         reply->deleteLater();
                         nam->deleteLater();
                         if (reply->error() == QNetworkReply::NoError) {
                             onOk(reply->readAll());
                         } else {
                             onErr(reply->errorString());
                         }
                     });
}
