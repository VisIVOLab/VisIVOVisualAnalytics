#ifndef SkavaClient_h
#define SkavaClient_h

#include <QObject>
#include <QString>
#include <QVector>
#include <optional>

/// Synchronous (worker-thread friendly) REST client for the SKAVA Data
/// Discovery and Access service.
///
/// Two roles, mirroring the architectural separation enshrined in D3.1:
///
///   * **Discovery**: ``searchDiscovery()`` queries ``GET /discovery/search``
///     with ObsCore-style scientific filters (POS, BAND, TIME, COLLECTION,
///     DPTYPE, obs_id) and returns a paginated result set. Discovery never
///     resolves the underlying file location — that is a separate concern.
///   * **Access**: ``getDatalink()`` queries ``GET /datalink/{obs_id}``
///     and returns the canonical DataLink JSON envelope, whose
///     ``primary_access.access_url`` is the URL the desktop should hand
///     to the VisIVO backend's ``/v1/datasets/open_url`` for download.
///
/// Both calls are blocking — designed to be invoked from a
/// ``QtConcurrent::run`` worker thread to keep the UI responsive. The
/// underlying HTTP machinery is shared across the codebase
/// (``QNetworkAccessManager`` via a thread-local helper installed by
/// ``BackendClient``).
///
/// SKAVA URL + bearer token come from ``Settings::getSkavaUrl()`` /
/// ``Settings::getSkavaToken()`` on construction; callers may pass an
/// override pair for unit tests or one-shot remote queries.

// ── Result structs ──────────────────────────────────────────────────────────

struct SkavaReplica
{
    QString code;
    QString name;
    bool    isEnabled{ true };
    bool    isAvailable{ true };
    double  latencyScore{ 0. };
    double  loadScore{ 0. };
    double  capabilityScore{ 0. };
    QString accessEndpoint;
};

struct SkavaDatasetMetadata
{
    QString obsId;
    QString obsCollection;
    QString obsTitle;
    QString dataproductType;
    int     calibLevel{ 0 };
    QString targetName;
    QString facilityName;
    QString instrumentName;
    /// All fields below are NaN when absent on the server side.
    double  sRa{ std::numeric_limits<double>::quiet_NaN() };
    double  sDec{ std::numeric_limits<double>::quiet_NaN() };
    double  sFov{ std::numeric_limits<double>::quiet_NaN() };
    double  tMin{ std::numeric_limits<double>::quiet_NaN() };
    double  tMax{ std::numeric_limits<double>::quiet_NaN() };
    double  emMin{ std::numeric_limits<double>::quiet_NaN() };
    double  emMax{ std::numeric_limits<double>::quiet_NaN() };
    QString spatialIndexCell;
    QString doi;
    QString pid;
    QString citation;
    QString license;
};

struct SkavaDiscoveryResult
{
    SkavaDatasetMetadata     metadata;
    QVector<SkavaReplica>    replicas;
    QString                  bestNode;
    /// Pre-resolved access URL exposed by SKAVA in the search payload — a
    /// convenience for client UIs that want to show "where will this come
    /// from" without making a second round-trip. The canonical access
    /// URL still comes from ``getDatalink()``.
    QString                  accessUrl;
};

struct SkavaDiscoveryResponse
{
    bool                          valid{ false };
    QString                       error;
    QVector<SkavaDiscoveryResult> results;
    int                           total{ 0 };
    int                           limit{ 0 };
    int                           offset{ 0 };
    int                           returned{ 0 };
};

struct SkavaDatalinkResponse
{
    bool                  valid{ false };
    QString               error;
    /// The download URL the desktop should hand to ``open_url`` — taken
    /// from ``primary_access.access_url``.
    QString               primaryAccessUrl;
    QString               primaryAccessFormat;
    qint64                primaryAccessEstSize{ 0 };
    QString               bestNode;
    SkavaDatasetMetadata  dataset;
    QVector<SkavaReplica> replicas;
};

// ── Search filter ───────────────────────────────────────────────────────────

struct SkavaSearchFilter
{
    /// POS=CIRCLE <ra> <dec> <radius>. Set all three when in use; leave
    /// at NaN to omit the spatial filter entirely (passing NaN to the
    /// server would be rejected by ``parse_pos`` per D3.1 §6.2).
    double posRa{ std::numeric_limits<double>::quiet_NaN() };
    double posDec{ std::numeric_limits<double>::quiet_NaN() };
    double posRadius{ std::numeric_limits<double>::quiet_NaN() };

    /// BAND=<min> <max> in METRES (wavelengths). For radio data the
    /// caller is expected to convert from Hz/MHz/GHz to lambda before
    /// populating these fields. NaN omits the filter.
    double bandMinMetres{ std::numeric_limits<double>::quiet_NaN() };
    double bandMaxMetres{ std::numeric_limits<double>::quiet_NaN() };

    /// TIME=<min> <max> in MJD. NaN omits the filter.
    double timeMinMjd{ std::numeric_limits<double>::quiet_NaN() };
    double timeMaxMjd{ std::numeric_limits<double>::quiet_NaN() };

    QString collection;
    /// ObsCore dataproduct type: "image", "cube", "timeseries",
    /// "spectrum", etc. Empty to omit.
    QString dataproductType;
    QString obsId;
    QString spatialCell;

    int limit{ 100 };
    int offset{ 0 };
};

class SkavaClient : public QObject
{
    Q_OBJECT
public:
    explicit SkavaClient(const QString &baseUrl = QString(),
                          const QString &bearerToken = QString(),
                          QObject *parent = nullptr);
    ~SkavaClient() override;

    /// Reconfigure the endpoint. Cheap — no network activity.
    void setBaseUrl(const QString &url);
    void setBearerToken(const QString &token);
    QString baseUrl() const { return m_baseUrl; }

    /// Blocking discovery query. ``GET /discovery/search`` with the supplied
    /// filter encoded as query parameters per ``discovery_queries.md``.
    SkavaDiscoveryResponse searchDiscovery(const SkavaSearchFilter &filter) const;

    /// Blocking access resolution. ``GET /datalink/{obs_id}`` returning the
    /// canonical DataLink JSON envelope. The desktop hands
    /// ``primaryAccessUrl`` to the VisIVO backend's ``/v1/datasets/open_url``
    /// to download and open the file.
    SkavaDatalinkResponse  getDatalink(const QString &obsId) const;

    // ── JSON-only parsers (factored out for unit-testing without HTTP) ──
    static SkavaDatasetMetadata    parseDatasetMetadata(const class QJsonObject &obj);
    static SkavaReplica            parseReplica(const class QJsonObject &obj);
    static SkavaDiscoveryResult    parseDiscoveryResult(const class QJsonObject &obj);
    static SkavaDiscoveryResponse  parseDiscoveryResponse(const class QJsonObject &obj);
    static SkavaDatalinkResponse   parseDatalinkResponse(const class QJsonObject &obj);

private:
    QByteArray performGet(const class QUrl &url, QString &error) const;

    QString m_baseUrl;
    QString m_bearerToken;
};

#endif  // SkavaClient_h
