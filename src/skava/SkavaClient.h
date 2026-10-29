#ifndef SkavaClient_h
#define SkavaClient_h

#include <atomic>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>
#include <limits>
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
/// ``QtConcurrent::run`` worker thread to keep the UI responsive, with a
/// ``QNetworkAccessManager`` of the calling thread's own (a thread-local
/// helper of this file).
///
/// The service URL and bearer token are given at construction (the SKAVA
/// tab reads them from its connection bar, a reopen from the Recent entry);
/// a client is made for one request and shared with the worker running it.

// ── Result structs ──────────────────────────────────────────────────────────

struct SkavaReplica
{
    QString code;
    QString name;
    bool    isEnabled{ true };
    bool    isAvailable{ true };
    /// Ranking scores entered by the SKAVA operator. NaN when the node has
    /// none: they are dimensionless ranking inputs, not measured latencies.
    double  latencyScore{ std::numeric_limits<double>::quiet_NaN() };
    double  loadScore{ std::numeric_limits<double>::quiet_NaN() };
    double  capabilityScore{ std::numeric_limits<double>::quiet_NaN() };
    QString accessEndpoint;
    /// SKAVA's routing decision for this replica: why the policy ranked it
    /// where it is (one line), its cost under the policy (lower is better;
    /// NaN for the static policy) and the age of the node telemetry it used.
    QString rankReason;
    double  score{ std::numeric_limits<double>::quiet_NaN() };
    double  telemetryAgeS{ std::numeric_limits<double>::quiet_NaN() };
};

/// A replica the ranking policy set aside, with why (a node that is down,
/// or one without the memory for this dataset).
struct SkavaExcludedReplica
{
    QString code;
    QString reason;
};

/// A service a node offers for this dataset, next to its replica: the node's
/// SODA cutout (synchronous or as a UWS job) and the products it computes
/// (moment map, PV diagram) as SODA jobs with a custom OPERATION. From the
/// JSON ``service_descriptors`` (``node-soda-sync``, ``node-soda-async``,
/// ``visivo-moment``, ``visivo-pv``) or the VOTable's ``adhoc:service``
/// RESOURCEs behind the ``#cutout`` / ``#proc`` rows.
struct SkavaNodeService
{
    /// "cutout-sync" | "cutout-async" | "moment" | "pv"
    QString kind;
    QString id;             ///< service_id / RESOURCE ID
    QString standardId;     ///< ivo://ivoa.net/std/SODA#sync-1.0 or #async-1.0
    QString accessUrl;      ///< the node's /soda/sync or /soda/async
    QString nodeCode;
    bool    requiresAuth{ false };   ///< the node's backend wants its token
    /// Inputs the service fixes for this dataset: ``ID`` (the replica's own
    /// identifier on that node) and, for a product, ``OPERATION``.
    QHash<QString, QString> inputParams;
    QString description;
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
    /// ObsCore ``access_format`` (VO protocol). When it is the DataLink
    /// content type, ``accessUrl`` is the dataset's DataLink document.
    QString                  accessFormat;
};

/// What a 401 asked for: the ``WWW-Authenticate`` challenge. SRCNet services
/// answer ``Bearer realm="…", ivoa_bearer standard_id="ivo://ivoa.net/sso#BearerToken",
/// access_url="<OIDC discovery document>"``: a bearer token from that IAM.
struct SkavaAuthChallenge
{
    bool    required{ false };   ///< the request was refused for want of a token
    QString scheme;              ///< "ivoa_bearer" when the IVOA challenge is there, else "bearer"
    QString realm;
    QString wellKnownUrl;        ///< the IAM's OpenID discovery document (ivoa_bearer access_url)
};

struct SkavaDiscoveryResponse
{
    bool                          valid{ false };
    QString                       error;
    SkavaAuthChallenge            authChallenge;   ///< set when the service answered 401
    QVector<SkavaDiscoveryResult> results;
    int                           total{ 0 };
    int                           limit{ 0 };
    int                           offset{ 0 };
    int                           returned{ 0 };
};

/// Descriptor for a VisIVO backend co-located with a dataset at an SRC
/// node. Returned by SKAVA inside ``service_descriptors[]`` under
/// ``service_type == "visivo-backend"``. See
/// docs/distributed-backend-design.md §5.2.
struct SkavaVisivoBackendDescriptor
{
    QString endpoint;       ///< Base URL of the VisIVO backend at the SRC
    QString nodeCode;       ///< SKAVA Node.code, cross-references the registry
    bool    requiresAuth{ false };
    QStringList supportedKinds;  ///< e.g. ["image", "cube", "dynspec"]
};

struct SkavaDatalinkResponse
{
    bool                  valid{ false };
    QString               error;
    SkavaAuthChallenge    authChallenge;   ///< set when the service answered 401
    /// Where this came from: the service's base URL, the protocol spoken
    /// ("skava" / "vo") and, for the VO protocol, the TAP and DataLink
    /// endpoints used (empty = derived from the base URL), so what is
    /// recorded about the dataset names the catalogue that answered, not the
    /// one the settings point at now.
    QString               catalogueUrl;
    QString               protocol;
    QString               tapUrl;
    QString               datalinkUrl;
    /// The access URL of the replica on the BEST node (``primary_access``).
    /// Another node's copy of the file lives elsewhere: use
    /// ``accessUrlForNode()`` for any node other than ``bestNode``.
    QString               primaryAccessUrl;
    QString               primaryAccessFormat;
    qint64                primaryAccessEstSize{ 0 };   ///< bytes (0 = unknown)
    QString               bestNode;
    SkavaDatasetMetadata  dataset;
    /// The replicas in SKAVA's ranking order, each with the policy's reason.
    QVector<SkavaReplica> replicas;
    /// The ranking policy that decided ("static", "resource_aware"), and the
    /// replicas it set aside with why. Empty from a DataLink VOTable.
    QString               policy;
    QVector<SkavaExcludedReplica> excluded;
    /// What the nodes offer for this dataset, best node first: cutouts and
    /// computed products next to the data.
    QVector<SkavaNodeService> services;
    /// VisIVO backends advertised in ``service_descriptors[]``. When
    /// non-empty the desktop should route the open through a registered
    /// backend matching one of these descriptors (compute next to data);
    /// when empty the desktop falls back to download via the local
    /// backend's ``/v1/datasets/open_url``.
    QVector<SkavaVisivoBackendDescriptor> visivoBackends;
    /// SODA service advertised by the DataLink document (VOTable
    /// ``adhoc:service`` behind the ``#cutout`` row): its access URL and
    /// standardID. Empty when no cutout is offered.
    QString sodaServiceUrl;
    QString sodaStandardId;

    /// Where this dataset lives on node ``nodeCode``: the primary access URL
    /// for the best node, otherwise that node's replica ``access_endpoint``.
    /// Empty when SKAVA lists no replica on that node.
    QString accessUrlForNode(const QString &nodeCode) const;
    /// The replica on ``nodeCode``, or null.
    const SkavaReplica *replica(const QString &nodeCode) const;
    /// The first service of ``kind`` (see SkavaNodeService::kind), on
    /// ``nodeCode`` when given, else on the best-ranked node that has one.
    const SkavaNodeService *service(const QString &kind, const QString &nodeCode = {}) const;
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

/// Not a QObject: a client is made for one request, handed to the worker
/// that runs it (shared with the panel, which keeps it only to cancel), and
/// freed by whichever side lets go of it last, on either thread.
class SkavaClient
{
public:
    explicit SkavaClient(const QString &baseUrl = QString(),
                          const QString &bearerToken = QString());
    ~SkavaClient();

    /// Reconfigure the endpoint. Cheap — no network activity.
    void setBaseUrl(const QString &url);
    void setBearerToken(const QString &token);
    QString baseUrl() const { return m_baseUrl; }

    /// Which interface to speak (see Settings::getSkavaProtocol): SKAVA's
    /// JSON API, or the IVOA standards (TAP/ObsCore + DataLink VOTable).
    enum class Protocol { SkavaJson, Vo };
    void setProtocol(Protocol protocol) { m_protocol = protocol; }
    Protocol protocol() const { return m_protocol; }
    /// VO protocol endpoints; empty = derived from the base URL (SKAVA layout).
    void setTapUrl(const QString &url);
    void setDatalinkUrl(const QString &url);
    QString tapUrl() const;

    /// Blocking discovery query. ``GET /discovery/search`` with the supplied
    /// filter encoded as query parameters per ``discovery_queries.md``.
    SkavaDiscoveryResponse searchDiscovery(const SkavaSearchFilter &filter) const;

    /// Blocking access resolution. ``GET /datalink/{obs_id}`` returning the
    /// canonical DataLink JSON envelope. The desktop hands
    /// ``primaryAccessUrl`` to the VisIVO backend's ``/v1/datasets/open_url``
    /// to download and open the file.
    SkavaDatalinkResponse  getDatalink(const QString &obsId) const;

    /// Access resolution for one search result row. With the VO protocol the
    /// row's own ObsCore access_url is used — a DataLink document when
    /// access_format says so, otherwise a direct download — because obs_id
    /// need not identify a single product. Falls back to getDatalink(obs_id).
    SkavaDatalinkResponse  resolveAccess(const SkavaDiscoveryResult &row) const;

    /// "vo" → Vo, anything else → SkavaJson (Settings::getSkavaProtocol values).
    static Protocol protocolFromString(const QString &name);

    // ── VO protocol (static, unit-testable) ──
    /// ADQL for an ObsCore search equivalent to @p filter: cone with CONTAINS,
    /// band and time as interval overlaps, string columns quoted with ''
    /// doubled. Pagination is TOP (ADQL 2.0 has no OFFSET).
    static QString buildObscoreAdql(const SkavaSearchFilter &filter);
    /// TAP result (VOTable TABLEDATA, ObsCore columns) → discovery response.
    static SkavaDiscoveryResponse parseObscoreVotable(const QByteArray &votable);
    /// DataLink VOTable → the same response the JSON DataLink produces:
    /// ``#this`` rows are the replicas (best first, ``node_id`` = node code,
    /// ``rank_reason`` / ``score`` the decision), ``#auxiliary`` rows pointing
    /// at a VisIVO backend's ``/v1/node/status`` (or, from older SKAVA,
    /// ``/v1/health``) are the co-located backends, and the ``#cutout`` /
    /// ``#proc`` rows with a service descriptor are the node services.
    static SkavaDatalinkResponse parseDatalinkVotable(const QByteArray &votable,
                                                      const QString &obsId);
    /// The kind of node service a DataLink row describes (see
    /// SkavaNodeService::kind), or empty when it is none of them.
    static QString nodeServiceKind(const QString &semantics, const QString &standardId,
                                   const QString &operation);
    /// ObsCore access_format of a DataLink document.
    static QString datalinkContentType();

    // ── JSON-only parsers (factored out for unit-testing without HTTP) ──
    static SkavaDatasetMetadata    parseDatasetMetadata(const class QJsonObject &obj);
    static SkavaReplica            parseReplica(const class QJsonObject &obj);
    static SkavaDiscoveryResult    parseDiscoveryResult(const class QJsonObject &obj);
    static SkavaDiscoveryResponse  parseDiscoveryResponse(const class QJsonObject &obj);
    static SkavaDatalinkResponse   parseDatalinkResponse(const class QJsonObject &obj);

public:
    /// Abort the request currently in flight, if any.
    ///
    /// Safe to call from any thread — in practice the UI thread, while the
    /// blocking call runs on a QtConcurrent worker. The worker notices within
    /// ~200 ms and returns an error reading "cancelled", so the panel is free
    /// again immediately instead of waiting out the network deadline. The flag
    /// stays set until clearCancel(): a caller clears it before each request
    /// it submits, so a cancel that arrived after the previous request had
    /// finished does not kill the next one.
    void requestCancel();

    /// Clear a previous cancellation. Call it on the UI thread immediately
    /// BEFORE submitting a request: clearing it inside the worker loses a
    /// Cancel pressed while the task was still queued, which left the panel
    /// stuck on "Cancelling…" for the whole of a request nobody wanted.
    void clearCancel();

private:
    /// ``challenge``, when given, receives a 401's WWW-Authenticate;
    /// ``anonymous`` leaves the bearer token out (a public document).
    QByteArray performGet(const class QUrl &url, QString &error,
                          const QByteArray &accept = "application/json",
                          SkavaAuthChallenge *challenge = nullptr, bool anonymous = false) const;
    /// Stamp a response with this client's service and protocol.
    void stampOrigin(SkavaDatalinkResponse &r) const;
    SkavaDiscoveryResponse searchTap(const SkavaSearchFilter &filter) const;
    SkavaDatalinkResponse  getDatalinkVo(const QString &obsId) const;
    SkavaDatalinkResponse  fetchDatalinkVo(const class QUrl &url, const QString &obsId) const;

public:
    /// SKAVA's error envelope ``{"error": {"code", "message"}}`` as one line,
    /// or empty when ``body`` is not one. Factored out for unit tests.
    static QString parseErrorEnvelope(const QByteArray &body);
    /// A ``WWW-Authenticate`` header as the challenge it carries (RFC 7235
    /// list of schemes with ``name="value"`` parameters). ``required`` is set
    /// for any non-empty header.
    static SkavaAuthChallenge parseAuthChallenge(const QString &wwwAuthenticate);

    /// The OpenID discovery document at ``wellKnownUrl`` → its authorization
    /// and token endpoints (blocking; empty on failure, ``error`` says why).
    struct OidcEndpoints
    {
        QString authorizationUrl;
        QString tokenUrl;
        QString error;
        bool valid() const { return !authorizationUrl.isEmpty() && !tokenUrl.isEmpty(); }
    };
    OidcEndpoints fetchOidcEndpoints(const QString &wellKnownUrl) const;
    /// The discovery document's fields; factored out for unit tests.
    static OidcEndpoints parseOidcDocument(const QByteArray &body);

private:

    QString m_baseUrl;
    QString m_bearerToken;
    Protocol m_protocol{ Protocol::SkavaJson };
    QString m_tapUrl;
    QString m_datalinkUrl;
    /// Set by requestCancel(), read by the worker inside performGet().
    mutable std::atomic_bool m_cancelRequested{ false };
};

#endif  // SkavaClient_h
