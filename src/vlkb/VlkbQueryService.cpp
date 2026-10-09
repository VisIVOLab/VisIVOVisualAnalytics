#include "VlkbQueryService.h"
#include "auth/AuthWrapper.h"
#include "gui/VlkbPosString.h"

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
    searchPos(visivo::circlePosString(l, b, r));
}

void VlkbQueryService::searchRange(double l, double b, double dl, double db)
{
    searchPos(visivo::rangePosString(l, b, dl, db));
}

void VlkbQueryService::searchPos(const QString &pos)
{
    // ONE place builds a POS string, and it is visivo::*PosString (unit-tested).
    // This function used to build its own — with `l += 180` for a negative
    // longitude instead of 360 — so after the caller's copy was fixed the panel
    // previewed 358° while the inventory was searched at 178°, and the cutout
    // that followed came from a third place again.
    QUrlQuery q;
    q.addQueryItem(u"POS"_s, pos);
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
