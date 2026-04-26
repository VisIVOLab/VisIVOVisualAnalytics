// SampClient.cpp – Qt SAMP Standard Profile client implementation
//
// SAMP XML-RPC wire format (subset used here):
//   Hub URL   : http://127.0.0.1:<port>/  (from ~/.samp lockfile)
//   Content-Type: text/xml
//   Method calls: samp.hub.register, samp.hub.declareMetadata,
//                 samp.hub.declareSubscriptions, samp.hub.notify,
//                 samp.hub.notifyAll, samp.hub.getRegisteredClients,
//                 samp.hub.getMetadata, samp.hub.getSubscriptions,
//                 samp.hub.ping, samp.hub.unregister

#include "SampClient.h"
#include "BackendClient.h"

#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// ── Internal HTTP helpers ──────────────────────────────────────────────────────

namespace {

/// Build a QNetworkRequest for the VisIVO backend (adds X-Visivo-Token).
QNetworkRequest buildBackendRequest(const QUrl &url, const QString &token)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QByteArrayLiteral("application/json"));
    if (!token.isEmpty())
        req.setRawHeader(QByteArrayLiteral("X-Visivo-Token"), token.toUtf8());
    return req;
}

/// Synchronous HTTP GET via *nam* with a 5 s timeout.  Returns response bytes.
QByteArray syncGet(QNetworkAccessManager *nam, const QUrl &url, const QString &token,
                   QString &errorOut)
{
    QNetworkRequest req = buildBackendRequest(url, token);
    QEventLoop loop;
    QNetworkReply *reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer t;
    t.setSingleShot(true);
    QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start(5000);
    loop.exec();
    if (!t.isActive()) { reply->abort(); errorOut = "GET timeout"; reply->deleteLater(); return {}; }
    t.stop();
    if (reply->error() != QNetworkReply::NoError) { errorOut = reply->errorString(); reply->deleteLater(); return {}; }
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

/// Synchronous HTTP POST via *nam* with a 5 s timeout.  Returns response bytes.
QByteArray syncPost(QNetworkAccessManager *nam, const QUrl &url, const QString &token,
                    const QByteArray &body, QString &errorOut)
{
    QNetworkRequest req = buildBackendRequest(url, token);
    req.setHeader(QNetworkRequest::ContentLengthHeader, body.size());
    QEventLoop loop;
    QNetworkReply *reply = nam->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer t;
    t.setSingleShot(true);
    QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start(5000);
    loop.exec();
    if (!t.isActive()) { reply->abort(); errorOut = "POST timeout"; reply->deleteLater(); return {}; }
    t.stop();
    if (reply->error() != QNetworkReply::NoError) { errorOut = reply->errorString(); reply->deleteLater(); return {}; }
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

} // namespace

// ── Constructor / Destructor ──────────────────────────────────────────────────

SampClient::SampClient(BackendClient *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_nam(new QNetworkAccessManager(this))
    , m_pollTimer(new QTimer(this))
    , m_backendPendingTimer(new QTimer(this))
{
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &SampClient::onPollTimer);

    m_backendPendingTimer->setInterval(kBackendPollIntervalMs);
    connect(m_backendPendingTimer, &QTimer::timeout,
            this, &SampClient::onBackendPendingTimer);
}

SampClient::~SampClient()
{
    disconnectFromHub();
}

// ── Connection lifecycle ──────────────────────────────────────────────────────

void SampClient::connectToHub()
{
    if (m_connected) {
        qDebug() << "[SampClient] Already connected to Hub.";
        return;
    }

    QString err;
    if (!readLockFile(m_hubUrl, m_sampSecret, err)) {
        qWarning() << "[SampClient] Hub lockfile not found:" << err;
        emit error(QStringLiteral("SAMP Hub not found: ") + err);
        return;
    }

    qDebug() << "[SampClient] Hub URL:" << m_hubUrl;

    if (!registerWithHub(err)) {
        qWarning() << "[SampClient] Registration failed:" << err;
        emit error(QStringLiteral("SAMP registration failed: ") + err);
        return;
    }

    declareMetadata();
    declareSubscriptions();

    m_connected = true;
    m_pollTimer->start();
    m_backendPendingTimer->start();

    reportStatusToBackend();
    emit connected(m_publicId, m_hubUrl);
    qDebug() << "[SampClient] Connected as" << m_publicId;
}

void SampClient::disconnectFromHub()
{
    if (!m_connected)
        return;

    m_pollTimer->stop();
    m_backendPendingTimer->stop();

    QString err;
    xmlRpcCall(QStringLiteral("samp.hub.unregister"),
               { m_privateKey }, err);

    m_connected   = false;
    m_privateKey  = QString();
    m_publicId    = QString();

    reportStatusToBackend();
    emit disconnected();
    qDebug() << "[SampClient] Disconnected from Hub.";
}

// ── Sending messages ──────────────────────────────────────────────────────────

void SampClient::notifyAll(const QString &mtype, const QVariantMap &params)
{
    if (!m_connected) {
        qWarning() << "[SampClient] notifyAll called while not connected.";
        return;
    }
    QVariantMap message;
    message[QStringLiteral("samp.mtype")]  = mtype;
    message[QStringLiteral("samp.params")] = params;

    QString err;
    xmlRpcCall(QStringLiteral("samp.hub.notifyAll"),
               { m_privateKey, message }, err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] notifyAll error:" << err;
}

void SampClient::notify(const QString &recipientId,
                        const QString &mtype,
                        const QVariantMap &params)
{
    if (!m_connected) return;
    QVariantMap message;
    message[QStringLiteral("samp.mtype")]  = mtype;
    message[QStringLiteral("samp.params")] = params;

    QString err;
    xmlRpcCall(QStringLiteral("samp.hub.notify"),
               { m_privateKey, recipientId, message }, err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] notify error:" << err;
}

void SampClient::sendImageFits(const QString &url, const QString &name,
                               const QString &recipientId)
{
    QVariantMap params;
    params[QStringLiteral("url")]       = url;
    params[QStringLiteral("name")]      = name.isEmpty() ? url : name;
    params[QStringLiteral("samp.name")] = params[QStringLiteral("name")];

    if (recipientId.isEmpty())
        notifyAll(QStringLiteral(MTYPE_IMAGE_FITS), params);
    else
        notify(recipientId, QStringLiteral(MTYPE_IMAGE_FITS), params);
}

void SampClient::sendTableVotable(const QString &url, const QString &name,
                                  const QString &tableId,
                                  const QString &recipientId)
{
    QVariantMap params;
    params[QStringLiteral("url")]      = url;
    params[QStringLiteral("name")]     = name.isEmpty() ? url : name;
    params[QStringLiteral("table-id")] = tableId.isEmpty() ? url : tableId;

    if (recipientId.isEmpty())
        notifyAll(QStringLiteral(MTYPE_TABLE_VOTABLE), params);
    else
        notify(recipientId, QStringLiteral(MTYPE_TABLE_VOTABLE), params);
}

void SampClient::sendPointAtSky(double raDeg, double decDeg,
                                const QString &recipientId)
{
    QVariantMap params;
    params[QStringLiteral("ra")]  = QString::number(raDeg,  'f', 6);
    params[QStringLiteral("dec")] = QString::number(decDeg, 'f', 6);

    if (recipientId.isEmpty())
        notifyAll(QStringLiteral(MTYPE_COORD_SKY), params);
    else
        notify(recipientId, QStringLiteral(MTYPE_COORD_SKY), params);
}

// ── Hub client list ───────────────────────────────────────────────────────────

QList<SampClientInfo> SampClient::registeredClients() const
{
    QList<SampClientInfo> result;
    if (!m_connected)
        return result;

    QString err;
    QVariant reg = xmlRpcCall(QStringLiteral("samp.hub.getRegisteredClients"),
                              { m_privateKey }, err);
    if (!err.isEmpty() || !reg.isValid())
        return result;

    const QVariantList ids = reg.toList();
    for (const QVariant &idv : ids) {
        const QString id = idv.toString();
        if (id == m_publicId)
            continue;   // skip self

        SampClientInfo info;
        info.id = id;

        QVariant meta = xmlRpcCall(QStringLiteral("samp.hub.getMetadata"),
                                   { m_privateKey, id }, err);
        if (err.isEmpty() && meta.isValid()) {
            const QVariantMap m = meta.toMap();
            info.name        = m.value(QStringLiteral("samp.name")).toString();
            info.description = m.value(QStringLiteral("samp.description.text")).toString();
            info.iconUrl     = m.value(QStringLiteral("samp.icon.url")).toString();
        }

        QVariant subs = xmlRpcCall(QStringLiteral("samp.hub.getSubscriptions"),
                                   { m_privateKey, id }, err);
        if (err.isEmpty() && subs.isValid()) {
            for (const QString &k : subs.toMap().keys())
                info.subscriptions << k;
        }

        result.append(info);
    }
    return result;
}

// ── Timer slots ───────────────────────────────────────────────────────────────

void SampClient::onPollTimer()
{
    pollHub();
}

void SampClient::onBackendPendingTimer()
{
    drainBackendPendingQueue();
}

// ── Polling / inbound ─────────────────────────────────────────────────────────

void SampClient::pollHub()
{
    // samp.hub.getCallbacks returns a list of pending calls.
    // For Standard Profile without a callback URL we use the simpler
    // samp.hub.getPendingCalls (introduced in SAMP 1.3).
    // Fall back gracefully if not supported.
    if (!m_connected) return;

    QString err;
    QVariant v = xmlRpcCall(QStringLiteral("samp.hub.getPendingCalls"),
                            { m_privateKey }, err);
    if (!err.isEmpty()) {
        // Hub may have shut down
        if (err.contains(QStringLiteral("connect"), Qt::CaseInsensitive) ||
            err.contains(QStringLiteral("refused"), Qt::CaseInsensitive)) {
            qWarning() << "[SampClient] Hub unreachable, disconnecting.";
            m_connected = false;
            m_pollTimer->stop();
            m_backendPendingTimer->stop();
            reportStatusToBackend();
            emit disconnected();
        }
        return;
    }

    // Each entry is a map: {msg-id, samp.mtype, samp.params, sender-id}
    const QVariantList calls = v.toList();
    for (const QVariant &cv : calls) {
        const QVariantMap call = cv.toMap();
        SampMessage msg;
        msg.msgId    = call.value(QStringLiteral("msg-id")).toString();
        msg.senderId = call.value(QStringLiteral("sender-id")).toString();
        const QVariantMap message = call.value(QStringLiteral("message")).toMap();
        msg.mtype  = message.value(QStringLiteral("samp.mtype")).toString();
        msg.params = message.value(QStringLiteral("samp.params")).toMap();

        // Reply OK to the Hub (fire-and-forget acknowledgement)
        if (!msg.msgId.isEmpty()) {
            QVariantMap reply;
            reply[QStringLiteral("samp.status")] = QStringLiteral("samp.ok");
            reply[QStringLiteral("samp.result")] = QVariantMap();
            xmlRpcCall(QStringLiteral("samp.hub.reply"),
                       { m_privateKey, msg.msgId, reply }, err);
        }

        // Hub shutdown event: clean up
        if (msg.mtype == QStringLiteral(MTYPE_HUB_SHUTDOWN)) {
            m_connected = false;
            m_pollTimer->stop();
            m_backendPendingTimer->stop();
            reportStatusToBackend();
            emit disconnected();
            return;
        }

        qDebug() << "[SampClient] Received mtype=" << msg.mtype
                 << "from" << msg.senderId;

        emit messageReceived(msg);
        forwardToBackend(msg);
    }
}

void SampClient::forwardToBackend(const SampMessage &msg)
{
    // POST /v1/samp/receive
    QJsonObject body;
    body[QStringLiteral("mtype")]     = msg.mtype;
    body[QStringLiteral("sender_id")] = msg.senderId;
    body[QStringLiteral("msg_id")]    = msg.msgId;

    QJsonObject paramsObj;
    for (auto it = msg.params.constBegin(); it != msg.params.constEnd(); ++it)
        paramsObj[it.key()] = QJsonValue::fromVariant(it.value());
    body[QStringLiteral("params")] = paramsObj;

    QString err;
    syncPost(m_nam,
             QUrl(m_backend->baseUrl() + QStringLiteral("/v1/samp/receive")),
             m_backend->token(),
             QJsonDocument(body).toJson(QJsonDocument::Compact),
             err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] forwardToBackend error:" << err;
}

// ── Backend outbound queue ────────────────────────────────────────────────────

void SampClient::drainBackendPendingQueue()
{
    if (!m_connected) return;

    QString err;
    const QByteArray payload = syncGet(
        m_nam,
        QUrl(m_backend->baseUrl() + QStringLiteral("/v1/samp/pending")),
        m_backend->token(),
        err);
    if (!err.isEmpty() || payload.isEmpty())
        return;

    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    const QJsonArray messages = root.value(QStringLiteral("messages")).toArray();
    for (const QJsonValue &mv : messages) {
        const QJsonObject msg = mv.toObject();
        const QString mtype    = msg.value(QStringLiteral("mtype")).toString();
        const QString targetId = msg.value(QStringLiteral("target_id")).toString();
        const QJsonObject rawParams = msg.value(QStringLiteral("params")).toObject();

        QVariantMap params;
        for (const QString &k : rawParams.keys()) {
            QString val = rawParams.value(k).toString();
            // Expand backend URL placeholder so VO tools can fetch the file
            val = expandBackendUrl(val);
            params[k] = val;
        }

        if (targetId.isEmpty())
            notifyAll(mtype, params);
        else
            notify(targetId, mtype, params);
    }
}

QString SampClient::expandBackendUrl(const QString &tmpl) const
{
    const QString placeholder = QStringLiteral("__BACKEND_BASE_URL__");
    if (!tmpl.contains(placeholder))
        return tmpl;
    return QString(tmpl).replace(placeholder, m_backend->baseUrl());
}

// ── Registration helpers ──────────────────────────────────────────────────────

bool SampClient::registerWithHub(QString &errorOut)
{
    QVariant result = xmlRpcCall(
        QStringLiteral("samp.hub.register"),
        { m_sampSecret },
        errorOut);

    if (!errorOut.isEmpty() || !result.isValid())
        return false;

    const QVariantMap map = result.toMap();
    m_privateKey = map.value(QStringLiteral("samp.private-key")).toString();
    m_publicId   = map.value(QStringLiteral("samp.self-id")).toString();

    if (m_privateKey.isEmpty() || m_publicId.isEmpty()) {
        errorOut = QStringLiteral("Hub returned empty private-key or self-id.");
        return false;
    }
    return true;
}

void SampClient::declareMetadata()
{
    QVariantMap meta;
    meta[QStringLiteral("samp.name")]             = QStringLiteral("VisIVO");
    meta[QStringLiteral("samp.description.text")] =
        QStringLiteral("VisIVO Visual Analytics — astrophysical data viewer");
    meta[QStringLiteral("samp.icon.url")] =
        m_backend->baseUrl() + QStringLiteral("/static/visivo_icon.png");
    meta[QStringLiteral("author.name")]   = QStringLiteral("INAF-OAC VisIVO Team");

    QString err;
    xmlRpcCall(QStringLiteral("samp.hub.declareMetadata"),
               { m_privateKey, meta }, err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] declareMetadata error:" << err;
}

void SampClient::declareSubscriptions()
{
    // Each entry in the subscriptions map is mtype → {} (empty params dict)
    QVariantMap subs;
    const QStringList supported = {
        QStringLiteral(MTYPE_IMAGE_FITS),
        QStringLiteral(MTYPE_TABLE_VOTABLE),
        QStringLiteral(MTYPE_TABLE_FITS),
        QStringLiteral(MTYPE_COORD_SKY),
        QStringLiteral(MTYPE_HUB_SHUTDOWN),
    };
    for (const QString &mt : supported)
        subs[mt] = QVariantMap();

    QString err;
    xmlRpcCall(QStringLiteral("samp.hub.declareSubscriptions"),
               { m_privateKey, subs }, err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] declareSubscriptions error:" << err;
}

void SampClient::reportStatusToBackend()
{
    // POST /v1/samp/hub-status
    const QList<SampClientInfo> clients = m_connected ? registeredClients()
                                                      : QList<SampClientInfo>{};
    QJsonArray clientsArr;
    for (const SampClientInfo &c : clients) {
        QJsonObject obj;
        obj[QStringLiteral("id")]   = c.id;
        obj[QStringLiteral("name")] = c.name;
        clientsArr.append(obj);
    }

    QJsonObject body;
    body[QStringLiteral("connected")]          = m_connected;
    body[QStringLiteral("hub_url")]            = m_hubUrl;
    body[QStringLiteral("client_id")]          = m_publicId;
    body[QStringLiteral("client_name")]        = QStringLiteral("VisIVO");
    body[QStringLiteral("registered_clients")] = clientsArr;

    QString err;
    syncPost(m_nam,
             QUrl(m_backend->baseUrl() + QStringLiteral("/v1/samp/hub-status")),
             m_backend->token(),
             QJsonDocument(body).toJson(QJsonDocument::Compact),
             err);
    if (!err.isEmpty())
        qWarning() << "[SampClient] reportStatusToBackend error:" << err;
}

// ── Hub lockfile ──────────────────────────────────────────────────────────────

bool SampClient::readLockFile(QString &hubUrl, QString &secret, QString &errorOut)
{
    // Standard lockfile path: ~/.samp  (SAMP 1.3 §2.1)
    const QString lockPath =
        QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
            .absoluteFilePath(QStringLiteral(".samp"));

    QFile f(lockPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorOut = QStringLiteral("Cannot open ~/.samp: ") + f.errorString();
        return false;
    }

    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('#')) || line.isEmpty())
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        const QString key = line.left(eq).trimmed();
        const QString val = line.mid(eq + 1).trimmed();
        if (key == QStringLiteral("samp.hub.xmlrpc.url"))
            hubUrl = val;
        else if (key == QStringLiteral("samp.secret"))
            secret = val;
    }

    if (hubUrl.isEmpty()) {
        errorOut = QStringLiteral("samp.hub.xmlrpc.url not found in ~/.samp");
        return false;
    }
    return true;
}

// ── XML-RPC implementation ────────────────────────────────────────────────────

QVariant SampClient::xmlRpcCall(const QString &method,
                                const QVariantList &params,
                                QString &errorOut) const
{
    const QByteArray body = buildXmlRpcRequest(method, params);

    QNetworkRequest req(QUrl(m_hubUrl));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QByteArrayLiteral("text/xml"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, body.size());

    // Perform synchronous call using a local event loop (called from non-main threads only
    // for the poll timer; the constructor runs on the main thread but timers are OK).
    QEventLoop loop;
    QNetworkReply *reply = m_nam->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // Timeout: 4 s
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(4000);

    loop.exec();

    if (timeout.isActive()) {
        timeout.stop();
    } else {
        reply->abort();
        errorOut = QStringLiteral("XML-RPC call timed out: ") + method;
        reply->deleteLater();
        return {};
    }

    if (reply->error() != QNetworkReply::NoError) {
        errorOut = reply->errorString();
        reply->deleteLater();
        return {};
    }

    const QByteArray response = reply->readAll();
    reply->deleteLater();

    return parseXmlRpcResponse(response, errorOut);
}

QByteArray SampClient::buildXmlRpcRequest(const QString &method,
                                          const QVariantList &params)
{
    QString xml;
    xml += QStringLiteral("<?xml version=\"1.0\"?>\n");
    xml += QStringLiteral("<methodCall>\n");
    xml += QStringLiteral("  <methodName>") + method +
           QStringLiteral("</methodName>\n");
    xml += QStringLiteral("  <params>\n");
    for (const QVariant &p : params) {
        xml += QStringLiteral("    <param><value>");
        xml += variantToXmlRpcValue(p);
        xml += QStringLiteral("</value></param>\n");
    }
    xml += QStringLiteral("  </params>\n");
    xml += QStringLiteral("</methodCall>\n");
    return xml.toUtf8();
}

QString SampClient::variantToXmlRpcValue(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Bool:
        return QStringLiteral("<boolean>") +
               (v.toBool() ? QStringLiteral("1") : QStringLiteral("0")) +
               QStringLiteral("</boolean>");
    case QMetaType::Int:
    case QMetaType::Long:
    case QMetaType::LongLong:
        return QStringLiteral("<i4>") + v.toString() + QStringLiteral("</i4>");
    case QMetaType::Double:
        return QStringLiteral("<double>") + v.toString() + QStringLiteral("</double>");
    case QMetaType::QVariantMap: {
        QString s = QStringLiteral("<struct>");
        const QVariantMap m = v.toMap();
        for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
            s += QStringLiteral("<member><name>") + it.key() +
                 QStringLiteral("</name><value>") +
                 variantToXmlRpcValue(it.value()) +
                 QStringLiteral("</value></member>");
        }
        s += QStringLiteral("</struct>");
        return s;
    }
    case QMetaType::QVariantList: {
        QString s = QStringLiteral("<array><data>");
        for (const QVariant &item : v.toList())
            s += QStringLiteral("<value>") + variantToXmlRpcValue(item) +
                 QStringLiteral("</value>");
        s += QStringLiteral("</data></array>");
        return s;
    }
    default:
        // Treat everything else as string, XML-escape the value
        return QStringLiteral("<string>") +
               v.toString()
                   .replace(QStringLiteral("&"),  QStringLiteral("&amp;"))
                   .replace(QStringLiteral("<"),  QStringLiteral("&lt;"))
                   .replace(QStringLiteral(">"),  QStringLiteral("&gt;"))
               + QStringLiteral("</string>");
    }
}

QVariant SampClient::parseXmlRpcResponse(const QByteArray &data, QString &errorOut)
{
    QDomDocument doc;
    QString domErr;
    if (!doc.setContent(data, &domErr)) {
        errorOut = QStringLiteral("XML-RPC parse error: ") + domErr;
        return {};
    }

    const QDomElement root = doc.documentElement(); // <methodResponse>

    // Check for <fault>
    const QDomElement fault = root.firstChildElement(QStringLiteral("fault"));
    if (!fault.isNull()) {
        const QVariant fv = xmlRpcValueToVariant(
            fault.firstChildElement(QStringLiteral("value")));
        const QVariantMap fm = fv.toMap();
        errorOut = fm.value(QStringLiteral("faultString"),
                            QStringLiteral("Unknown fault")).toString();
        return {};
    }

    // Normal response: <params><param><value>…
    const QDomElement params = root.firstChildElement(QStringLiteral("params"));
    const QDomElement param  = params.firstChildElement(QStringLiteral("param"));
    const QDomElement value  = param.firstChildElement(QStringLiteral("value"));
    if (value.isNull()) {
        errorOut = QStringLiteral("Empty XML-RPC response.");
        return {};
    }
    return xmlRpcValueToVariant(value);
}

QVariant SampClient::xmlRpcValueToVariant(const QDomElement &el)
{
    // <value> may contain a type child or raw text (implicit string)
    QDomElement child = el.firstChildElement();
    if (child.isNull()) {
        // implicit string
        return QVariant(el.text());
    }

    const QString tag = child.tagName();

    if (tag == QStringLiteral("string"))
        return QVariant(child.text());

    if (tag == QStringLiteral("int") || tag == QStringLiteral("i4") ||
        tag == QStringLiteral("i8"))
        return QVariant(child.text().toLongLong());

    if (tag == QStringLiteral("boolean"))
        return QVariant(child.text().trimmed() == QStringLiteral("1"));

    if (tag == QStringLiteral("double"))
        return QVariant(child.text().toDouble());

    if (tag == QStringLiteral("struct")) {
        QVariantMap map;
        QDomElement member = child.firstChildElement(QStringLiteral("member"));
        while (!member.isNull()) {
            const QString name =
                member.firstChildElement(QStringLiteral("name")).text();
            const QDomElement val =
                member.firstChildElement(QStringLiteral("value"));
            map[name] = xmlRpcValueToVariant(val);
            member = member.nextSiblingElement(QStringLiteral("member"));
        }
        return QVariant(map);
    }

    if (tag == QStringLiteral("array")) {
        QVariantList list;
        const QDomElement data = child.firstChildElement(QStringLiteral("data"));
        QDomElement val = data.firstChildElement(QStringLiteral("value"));
        while (!val.isNull()) {
            list.append(xmlRpcValueToVariant(val));
            val = val.nextSiblingElement(QStringLiteral("value"));
        }
        return QVariant(list);
    }

    // Nil / unknown
    return QVariant();
}
