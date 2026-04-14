#ifndef BackendClient_h
#define BackendClient_h

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <chrono>

#include <array>
#include <map>
#include <vector>

struct BackendFileEntry
{
    QString name;
    QString path;
    QString type;
    qint64 size{ 0 };
    QString modifiedTime;
    bool isFits{ false };
};

struct BackendHealthResult
{
    bool ok{ false };
    QString error;
    int workers{ 0 };
    int activeSessions{ 0 };
    int productCacheEntries{ 0 };
    int productCacheCapacity{ 0 };
    int taskRegistryEntries{ 0 };
    bool taskTtlEnabled{ false };
    int taskTtlSeconds{ 0 };
};

struct BackendListFilesResult
{
    bool valid{ false };
    QString error;
    QString currentPath;
    std::vector<BackendFileEntry> entries;
};

struct BackendOpenDatasetResult
{
    bool valid{ false };
    QString error;
    QString datasetId;
    QString sessionId;   // R3: echo as X-Visivo-Session in subsequent requests
    QString kind;
    int activeAxes{ 0 };
    int width{ 0 };
    int height{ 0 };
    int depth{ 0 };
    QString degenerateAxesSummary;
    QString wcsStatus{ QStringLiteral("ok") };
    QString wcsWarningMessage;
    QList<int> wcsSanitizedAxes;
    QString spectralAxisType;
    QString spectralAxisUnit;
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    std::array<double, 3> origin{ 0.0, 0.0, 0.0 };
    std::array<QString, 3> ctype{ QString(), QString(), QString() };
    std::array<QString, 3> cunit{ QString(), QString(), QString() };
    std::array<double, 3> crval{ 0.0, 0.0, 0.0 };
    std::array<double, 3> crpix{ 1.0, 1.0, 1.0 };
    std::array<double, 3> cdelt{ 1.0, 1.0, 1.0 };
};

struct BackendFileHeaderResult
{
    bool valid{ false };
    QString error;
    QStringList cards;
};

struct BackendMomentResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    QString scalarType;
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QString spectralAxisType; // CTYPE3 value, e.g. "FREQ", "VRAD"
    QString spectralAxisUnit; // CUNIT3 value, e.g. "Hz", "km/s"
    QString momentUnit;       // e.g. "Jy/beam Hz", "km/s", "km^2/s^2"
    QString bunit;            // raw BUNIT from the FITS header
    QString wcsStatus{ QStringLiteral("ok") }; // "ok", "sanitized", or "degraded"
    QString wcsWarningMessage;                  // human-readable WCS warning, empty when ok
    QByteArray data;
};

struct BackendTaskCreateResult
{
    bool valid{ false };
    QString error;
    QString taskId;
    QString status;
    bool cacheHit{ false };
};

struct BackendTaskStatusResult
{
    bool valid{ false };
    QString error;
    QString taskId;
    QString operation;
    QString status;
    double progress{ 0.0 };
    bool cacheHit{ false };
    QJsonObject resultObject;
};

struct BackendCubePreviewResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    int depth{ 0 };
    QString scalarType;
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QByteArray data;
};

struct BackendCubeSliceResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    QString scalarType;
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QByteArray data;
};

struct BackendCubeSubvolumeResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    int depth{ 0 };
    QString scalarType;
    QByteArray data;
};

struct BackendCubePvResult
{
    bool valid{ false };
    QString error;
    int numSamples{ 0 };
    int depth{ 0 };
    QString scalarType;
    QByteArray positions;        // pixel-distance offset along path (float32[N])
    QByteArray positionsArcsec;  // angular offset in arcseconds (float32[N])
    double pixelScaleArcsecPerPixel{ 0. };
    QString spatialUnit;         // "arcsec" when physical coords are available
    QByteArray data;
    QString computedOn;
    int widthPixels{ 1 };
    int vertexCount{ 0 };
    double totalLength{ 0. };
    int validSamples{ 0 };
    QString spectralAxisType;
    QString spectralAxisUnit;
    QString bunit;
    double beamMajor{ 0.0 };
    double beamMinor{ 0.0 };
    double beamPa{ 0.0 };
};

struct BackendCubeNoiseResult
{
    bool valid{ false };
    QString error;
    int channelStart{ 0 };
    int channelEnd{ 0 };
    int numChannels{ 0 };
    QVector<double> mad;
    QVector<double> sigma;
    int x0{ 0 };
    int x1{ 0 };
    int y0{ 0 };
    int y1{ 0 };
};

struct BackendCatalogueFieldInfo
{
    QString name;
    QString type;
};

struct BackendCatalogueInfo
{
    bool valid{ false };
    QString error;
    QString datasetId;
    QString sessionId;
    int numRows{ 0 };
    std::vector<BackendCatalogueFieldInfo> fields;
    QString raField;
    QString decField;
};

struct BackendCatalogueSubset
{
    bool valid{ false };
    QString error;
    QList<QVariantMap> rows;
};

// ── Tabular query (shared for catalogue + VBT point) ─────────────────────────

struct BackendTabularFilter
{
    QString field;
    QString op;    // "=", "!=", ">", ">=", "<", "<=", "contains", "in"
    QString value;
};

struct BackendTabularQueryRequest
{
    QString datasetId;
    int limit{ 100000 };
    int offset{ 0 };
    std::vector<BackendTabularFilter> filters;
    QString sortField;
    QString sortDirection; // "asc" or "desc", empty = no sort
};

struct BackendTabularQueryResponse
{
    bool valid{ false };
    QString error;
    int totalRows{ 0 };
    int returnedRows{ 0 };
    // Catalogue: row-oriented
    QList<QVariantMap> rows;
    // VBT point: column-oriented, decoded float64
    std::map<QString, std::vector<double>> columns;
    QStringList fieldNames;
    int numRows{ 0 };
};

// ── VBT (VisIVO Binary Table) ─────────────────────────────────────────────────

struct BackendVbtFieldInfo
{
    QString name;
    QString type; // "float32" or "float64"
};

struct BackendVbtOpenResult
{
    bool valid{ false };
    QString error;
    QString datasetId;
    QString sessionId;
    QString kind;        // "point" or "volume"
    std::vector<BackendVbtFieldInfo> fields;
    int numRows{ 0 };
    QString scalarType;  // "float32" or "float64"
    // volume only:
    std::array<int, 3> dimensions{ 0, 0, 0 };
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
};

struct BackendVbtSubsetResult
{
    bool valid{ false };
    QString error;
    QString kind;       // "point" or "volume"
    int numRows{ 0 };
    QStringList fieldNames;
    // point data: decoded float64 columns keyed by field name
    std::map<QString, std::vector<double>> columns;
    // volume data: single-field float32-LE block
    QString fieldName;
    QByteArray block;
    std::array<int, 3> dimensions{ 0, 0, 0 };
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
};

struct BackendImageResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    int fullWidth{ 0 };
    int fullHeight{ 0 };
    QString scalarType;
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    bool isPreview{ false };
    double previewScaleFactor{ 1.0 };
    QByteArray data;
};

struct BackendIsosurfaceResult
{
    bool valid{ false };
    QString error;
    int numPoints{ 0 };
    int numPolys{ 0 };
    QByteArray pointsData;
    QByteArray polysData;
};

// ── HiPS (Hierarchical Progressive Survey) ────────────────────────────────────

struct BackendHiPSSurveyInfo
{
    bool valid{ false };
    QString error;
    QString surveyId;
    QString sessionId;
    QString baseUrl;
    QString title;
    QString frame;           // "equatorial", "galactic", "ecliptic"
    int maxOrder{ 9 };
    int minOrder{ 0 };
    int tileWidth{ 512 };
    QStringList tileFormats; // e.g. {"png"} or {"jpeg"}
};

struct BackendHiPSTileInfo
{
    int pix{ 0 };
    int order{ 0 };
    double raCenterDeg{ 0.0 };
    double decCenterDeg{ 0.0 };
    std::array<double, 4> cornersRaDeg{};
    std::array<double, 4> cornersDecDeg{};
};

struct BackendHiPSViewResponse
{
    bool valid{ false };
    QString error;
    std::vector<BackendHiPSTileInfo> tiles;
};

// ── Milestone 2 additions ─────────────────────────────────────────────────────

struct BackendTargetResolveResult
{
    bool valid{ false };
    QString error;
    double raDeg{ 0.0 };
    double decDeg{ 0.0 };
    QString resolvedName;
    QString source;    // "sesame"
};

struct BackendHiPSCatalogueSource
{
    double raDeg{ 0.0 };
    double decDeg{ 0.0 };
};

// ── Cosmology ─────────────────────────────────────────────────────────────────

struct BackendCosmologyBatchResult
{
    bool valid{ false };
    QString error;
    QString model;
    QVector<double> distancesMpc; // same order as the submitted redshifts
};

struct BackendHiPSCatalogueOverlayResult
{
    bool valid{ false };
    QString error;
    QVector<BackendHiPSCatalogueSource> sources;
    int totalInView{ 0 };
    bool truncated{ false };
};

struct BackendVlkbFetchResult
{
    bool valid{ false };
    QString error;
    /// Absolute path on the *backend* filesystem where the FITS file was saved.
    QString path;
};

class QJsonDocument;
class QJsonObject;
class QUrl;

/**
 * HTTP client for the VisIVO FastAPI backend.
 *
 * Changes vs original:
 *   R1  – Every request carries the X-Visivo-Token header (auth).
 *   R3  – openDataset() stores the returned session_id; all subsequent
 *          requests carry X-Visivo-Session so the backend can isolate
 *          per-session state.
 *   R8  – All URL paths are prefixed with /v1/.
 *
 * The token is resolved in this priority order:
 *   1. setToken() call (e.g. from Settings dialog)
 *   2. VISIVO_TOKEN environment variable
 *   3. Content of ~/.visivo_token (written by the backend at startup)
 */
class BackendClient
{
public:
    /**
     * @param baseUrl  Backend base URL, e.g. "http://compute-node42:8000"
     * @param token    Auth token.  If empty, readTokenFile() is attempted.
     */
    explicit BackendClient(QString baseUrl = QStringLiteral("http://127.0.0.1:8000"),
                           QString token = QString());

    // ── Accessors ─────────────────────────────────────────────────────────────

    QString baseUrl() const;
    void setBaseUrl(const QString &url);

    /** Current auth token.  Empty means no authentication is sent. */
    QString token() const;
    void setToken(const QString &token);

    /**
     * Session ID returned by the last successful openDataset() call.
     * Sent as X-Visivo-Session on every subsequent request.
     */
    QString sessionId() const;
    void setSessionId(const QString &sessionId);

    // ── Static helpers ────────────────────────────────────────────────────────

    /**
     * Try to read the auth token from:
     *   1. VISIVO_TOKEN environment variable
     *   2. ~/.visivo_token (file written by the backend at startup)
     * Returns an empty string if neither source is available.
     */
    static QString readTokenFile();

    // ── API calls ─────────────────────────────────────────────────────────────

    BackendHealthResult health() const;
    BackendListFilesResult listFiles(const QString &path) const;
    BackendFileHeaderResult fileHeader(const QString &path) const;

    /**
     * Open a FITS dataset on the backend.
     * On success the returned sessionId is automatically stored in this client
     * so all subsequent calls carry the correct X-Visivo-Session header.
     */
    BackendOpenDatasetResult openDataset(const QString &path);
    BackendCatalogueInfo openCatalogue(const QString &path, const QString &format = QStringLiteral("csv"));

    BackendCubePreviewResult requestPreview(const QString &datasetId, int downsample) const;
    BackendCubeSliceResult requestSlice(const QString &datasetId, const QString &axis,
                                        int index) const;
    BackendCubeSubvolumeResult requestSubvolume(const QString &datasetId, int x0, int x1, int y0,
                                                int y1, int z0, int z1) const;
    BackendCubePvResult requestPv(const QString &datasetId,
                                  const std::vector<std::array<int, 2>> &vertices,
                                  int widthPixels) const;
    BackendCubeNoiseResult requestNoise(const QString &datasetId, int x0, int x1, int y0, int y1,
                                        int channelStart, int channelEnd) const;
    BackendCatalogueSubset requestCatalogueSubset(const QString &datasetId, int maxRows) const;
    BackendTabularQueryResponse queryTabularCatalogue(const BackendTabularQueryRequest &req) const;
    BackendVbtOpenResult openVbt(const QString &path, const QString &format = QString());
    BackendVbtSubsetResult requestVbtSubset(const QString &datasetId, int maxRows = 0,
                                            const QString &fieldName = QString()) const;
    BackendTabularQueryResponse queryTabularVbt(const BackendTabularQueryRequest &req) const;
    // ── HiPS endpoints ────────────────────────────────────────────────────────
    BackendHiPSSurveyInfo openHiPS(const QString &url);
    /** GET /v1/hips/{id}/allsky?order=N – raw PNG/JPEG bytes of the AllSky mosaic. */
    QByteArray requestHiPSAllsky(const QString &surveyId, int order) const;
    /** GET /v1/hips/{id}/tile/{order}/{pix} – raw PNG/JPEG bytes of one tile. */
    QByteArray requestHiPSTile(const QString &surveyId, int order, int pix) const;
    /** POST /v1/hips/{id}/query_tiles – tile list for the current viewport. */
    BackendHiPSViewResponse requestHiPSTilesForView(const QString &surveyId,
                                                    double raDeg, double decDeg,
                                                    double fovDeg, int order) const;
    // ── Milestone 2 ──────────────────────────────────────────────────────────
    // ── Cosmology endpoints ───────────────────────────────────────────────────
    /** POST /v1/cosmology/distance/batch – comoving distances for a list of redshifts. */
    BackendCosmologyBatchResult requestCosmologyDistanceBatch(const QVector<double> &redshifts,
                                                              const QString &model) const;

    /** POST /v1/resolve/target – resolve object name → RA/Dec via Sesame. */
    BackendTargetResolveResult resolveTarget(const QString &name) const;
    /** POST /v1/hips/catalogue_overlay – RA/Dec sources in current viewport. */
    BackendHiPSCatalogueOverlayResult requestHiPSCatalogueOverlay(
            const QString &cataloguePath, double raDeg, double decDeg, double fovDeg,
            const QString &raField = {}, const QString &decField = {},
            int maxSources = 5000) const;

    /** POST /v1/vlkb/fetch_cutout – download a VLKB SODA cutout server-side.
     *  @param sodaUrl     Full SODA sync URL (ID + POS + POSSYS already encoded).
     *  @param vlkbBearer  Optional VLKB bearer token forwarded in Authorization header.
     *  Returns the backend-local path of the saved FITS file on success. */
    BackendVlkbFetchResult vlkbFetchCutout(const QString &sodaUrl,
                                            const QString &vlkbBearer = {}) const;

    BackendImageResult requestImagePreview(const QString &datasetId, int maxLongestSide) const;
    BackendImageResult requestImage(const QString &datasetId) const;
    BackendIsosurfaceResult requestIsosurface(const QString &datasetId, double threshold) const;
    BackendMomentResult requestMoment(const QString &datasetId, int order, int channelStart,
                                      int channelEnd, bool maskEnabled,
                                      double thresholdValue) const;
    BackendTaskCreateResult createMomentTask(const QString &datasetId, int order, int channelStart,
                                             int channelEnd, bool maskEnabled,
                                             double thresholdValue) const;
    BackendTaskCreateResult createPvTask(const QString &datasetId,
                                         const std::vector<std::array<int, 2>> &vertices,
                                         int widthPixels) const;
    BackendTaskStatusResult requestTaskStatus(const QString &taskId) const;
    BackendTaskStatusResult waitForTaskCompletion(const BackendTaskCreateResult &createResult,
                                                  const QString &logTag,
                                                  int maxPollAttempts = 120,
                                                  int pollIntervalMs = 250) const;
    static BackendMomentResult    parseMomentResultObject(const QJsonObject &object);
    static BackendCubePvResult    parsePvResultObject(const QJsonObject &object);
    static BackendCubeNoiseResult parseNoiseResultObject(const QJsonObject &object);

private:
    /** Build a QNetworkRequest with auth + session headers pre-applied. */
    QNetworkRequest buildRequest(const QUrl &url) const;

    QByteArray performGet(const QUrl &url, QString &error) const;
    QByteArray performPost(const QUrl &url, const QJsonDocument &body, QString &error) const;
    std::chrono::milliseconds requestTimeoutFor(const QUrl &url) const;

    static QByteArray decodePayload(const QJsonObject &object, const QString &base64Field,
                                    const QString &compressionField, QString &error);

    QString m_baseUrl;
    QString m_token;
    QString m_sessionId;
};

#endif
