#ifndef BackendClient_h
#define BackendClient_h

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkRequest>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <chrono>

#include <array>
#include <limits>
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

// One finished task as reported by /v1/health (newest first). Used to render
// the "Recent operations" trail in the DataHub System Insights panel.
struct BackendRecentTask
{
    QString operation;        // "moment", "pv", "image_full", …
    QString status;           // "done" | "failed" | "cancelled"
    double durationSeconds{ 0.0 };
    double ageSeconds{ 0.0 }; // seconds since the task finished
    bool cacheHit{ false };
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
    // Live activity, populated from the new /v1/health snapshot fields.
    int runningTasks{ 0 };
    std::vector<BackendRecentTask> recentTasks;
};

struct BackendListFilesResult
{
    bool valid{ false };
    QString error;
    QString currentPath;
    std::vector<BackendFileEntry> entries;
};

struct BackendSessionDatasetEntry
{
    QString datasetId;
    QString kind;        ///< "cube" | "image" | "" if unknown
    QString path;
    int width{ 0 };
    int height{ 0 };
    int depth{ 0 };
};

struct BackendSessionDatasetsResult
{
    bool valid{ false };
    QString error;
    QString sessionId;
    std::vector<BackendSessionDatasetEntry> datasets;
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
    /// Pixel scale in arcsec from astropy (handles CD matrix / PC matrix).
    /// Prefer over sqrt(|CDELT1*CDELT2|)*3600 for layer alignment.
    double pixelScaleArcsec{ 0.0 };
    /// Effective per-axis CDELT in deg/pix WITH CORRECT SIGN, derived from a
    /// 1-pixel perturbation at CRPIX via astropy WCS.  Valid when effectiveCdeltValid=true.
    /// Unlike the raw cdelt field, this is correct even for CD-matrix-only FITS.
    std::array<double, 3> effectiveCdelt{ 1.0, 1.0, 1.0 };
    bool effectiveCdeltValid{ false };
    /// Sky coordinates (native WCS frame, degrees) of image pixels (0,0) and (0,100).
    /// Computed server-side by astropy to bypass CDELT limitations.
    std::array<double, 2> wcsRefSky00{ 0.0, 0.0 };
    std::array<double, 2> wcsRefSky0100{ 0.0, 0.0 };
    bool wcsRefSkyValid{ false };
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

/// Raw binary array result from the /binary transport endpoints.
/// \c data holds \c rows*cols*4 bytes of float32 LE pixel values, row-major.
/// NaN encodes masked / invalid pixels.
struct BinaryArrayResult
{
    int rows{ 0 };
    int cols{ 0 };
    QByteArray data;   // raw float32 bytes, rows*cols*4 bytes
    QString error;
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

struct BackendSampSendResult
{
    bool valid{ false };
    QString error;
    QString jobId;
    bool sentDirect{ false };
};

struct BackendSampImportResult
{
    bool valid{ false };
    QString error;
    QString token;
    QString backendPath;
    QString filename;
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

// ── S-02: Line-width maps ─────────────────────────────────────────────────────

struct BackendLinewidthResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    QString fwhmUnit;
    QString ewUnit;
    QString bunit;
    double fwhmRangeMin{ 0. };
    double fwhmRangeMax{ 0. };
    double ewRangeMin{ 0. };
    double ewRangeMax{ 0. };
    /// Two back-to-back binary frames: [FWHM frame][EW frame].
    /// Each frame: 12-byte big-endian header + float32 LE body.
    QByteArray binaryPayload;
};

// ── S-03: Baseline subtraction ────────────────────────────────────────────────

struct BackendBaselineResult
{
    bool valid{ false };
    QString error;
    QString newDatasetId;
    int width{ 0 };
    int height{ 0 };
    int depth{ 0 };
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    int polyOrder{ 0 };
    int fitChannels{ 0 };
    QString outputPath;
    double rmsBefore{ 0. };  ///< RMS of the original cube over the fit channels
    double rmsAfter{ 0. };   ///< RMS of the baseline-subtracted cube
    QString method;          ///< "polynomial" or "median"
};

// ── S-04: Spectral stacking ───────────────────────────────────────────────────

struct BackendStackResult
{
    bool valid{ false };
    QString error;
    /// Stacked 1-D spectrum: rows=1, cols=n_chan.
    BinaryArrayResult spectrum;
    QString method;
    int nCubes{ 0 };
};

// ── S-05: SoFiA-2 source finding ──────────────────────────────────────────────

struct BackendSourceFindSource
{
    QVariantMap fields;   // all SoFiA catalogue columns as key→QVariant
};

struct BackendSourceFindResult
{
    bool valid{ false };
    QString error;
    int nSources{ 0 };
    QVector<BackendSourceFindSource> sources;
};

// ── A-01: Aperture photometry ─────────────────────────────────────────────────

struct BackendAperturePhotometryResult
{
    bool valid{ false };
    QString error;
    double fluxJy{ 0. };
    double fluxErrJy{ 0. };
    double backgroundJyPerPix{ 0. };
    int nPixAperture{ 0 };
    int channel{ -1 };          // -1 means "not set"
    double pixX{ 0. };
    double pixY{ 0. };
    double pixelScaleArcsec{ 0. };
};

// ── A-02: 2-D Gaussian source fitting ────────────────────────────────────────

struct BackendGaussFit2DResult
{
    bool valid{ false };
    QString error;
    double peakJyBeam{ 0. };
    double integratedFluxJy{ 0. };
    double majorArcsec{ 0. };
    double minorArcsec{ 0. };
    double paDeg{ 0. };
    double deconvolvedMajorArcsec{ std::numeric_limits<double>::quiet_NaN() };
    double deconvolvedMinorArcsec{ std::numeric_limits<double>::quiet_NaN() };
    double fitRms{ 0. };
    double centerPixX{ 0. };
    double centerPixY{ 0. };
    double background{ 0. };
};

// ── A-03: WCS reprojection ────────────────────────────────────────────────────

struct BackendReprojectResult
{
    bool valid{ false };
    QString error;
    QString newDatasetId;
    int width{ 0 };
    int height{ 0 };
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QString outputPath;
    int order{ 1 };
};

// ── A-04: Catalogue cross-match ───────────────────────────────────────────────

struct BackendCrossmatchSource
{
    double raDeg{ 0. };
    double decDeg{ 0. };
    QString name;
    QString catalogueId;
    double separationArcsec{ 0. };
};

struct BackendCrossmatchResult
{
    bool valid{ false };
    QString error;
    int nSources{ 0 };
    bool truncated{ false };
    double centreRaDeg{ 0. };
    double centreDecDeg{ 0. };
    double searchRadiusDeg{ 0. };
    QVector<BackendCrossmatchSource> sources;
};

// ── P-01: Stokes plane extraction ────────────────────────────────────────────

struct BackendStokesPlaneResult
{
    bool valid{ false };
    QString error;
    QString plane;      ///< "I", "Q", "U", or "V"
    int channel{ 0 };
    int width{ 0 };
    int height{ 0 };
    float rangeMin{ 0.f };
    float rangeMax{ 0.f };
    QByteArray data;    ///< float32 LE, width × height, from binary endpoint
};

// ── P-02: Polarised intensity and position angle ──────────────────────────────

struct BackendPolarisationPiResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };
    int height{ 0 };
    int channel{ 0 };
    double sigmaQu2{ 0. };         ///< variance estimate σ²_QU used for bias correction
    float piRangeMin{ 0.f };
    float piRangeMax{ 0.f };
    float paRangeMin{ 0.f };
    float paRangeMax{ 0.f };
    QString piDatasetId;
    QString paDatasetId;
    QByteArray piData;             ///< float32 LE PI map (from /binary endpoint)
    QByteArray paData;             ///< float32 LE PA map (from /binary endpoint)
};

// ── P-03: Rotation Measure synthesis ─────────────────────────────────────────

struct BackendRmSynthesisResult
{
    bool valid{ false };
    QString error;
    int nPhi{ 0 };
    int nFreq{ 0 };
    int width{ 0 };
    int height{ 0 };
    double phiMin{ 0. };
    double phiMax{ 0. };
    double dPhi{ 0. };
    QVector<double> phiGrid;      ///< φ sample points in rad/m²
    QVector<double> rmsf;         ///< RM spread function |R(φ)|
    float rangeMin{ 0.f };
    float rangeMax{ 0.f };
    QByteArray fdfData;           ///< float32 LE FDF cube (nPhi*height rows × width cols)
};

// ── C-01: Angular power spectrum ─────────────────────────────────────────────

struct BackendAngularPowerResult
{
    bool valid{ false };
    QString error;
    int channel{ 0 };
    int width{ 0 };
    int height{ 0 };
    int nBins{ 0 };
    QVector<double> kBins;        ///< wavenumber bin centres (pixel⁻¹)
    QVector<double> power;        ///< mean power per bin (counts²)
    QString kUnit;
    QString powerUnit;
};

// ── C-03: Skeleton / void detection ──────────────────────────────────────────

struct BackendSkeletonResult
{
    bool valid{ false };
    QString error;
    QString install;              ///< non-empty when scikit-image is absent
    QString feature;
    int channel{ 0 };
    int width{ 0 };
    int height{ 0 };
    double threshold{ 0. };
    QString mode;                 ///< "skeleton" or "void"
    int nSkeletonPixels{ 0 };
    float rangeMin{ 0.f };
    float rangeMax{ 0.f };
    QByteArray data;              ///< float32 LE 0/255 mask from binary endpoint
};

// ── R-01: CARTA script export ─────────────────────────────────────────────────

struct BackendCartaExportResult
{
    bool valid{ false };
    QString error;
    QString script;               ///< Python script content
    int nDatasets{ 0 };
};

// ── R-02: PROV-JSON provenance ────────────────────────────────────────────────

struct BackendProvJsonResult
{
    bool valid{ false };
    QString error;
    QJsonObject prov;             ///< Raw W3C PROV-JSON document
};

// ── R-03: Parameter snapshots ─────────────────────────────────────────────────

struct BackendSnapshotEntry
{
    QString operation;
    QString datasetId;
    QString timestamp;
    QVariantMap params;
};

struct BackendSnapshotListResult
{
    bool valid{ false };
    QString error;
    int nEntries{ 0 };
    QVector<BackendSnapshotEntry> history;
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

    /// GET /v1/sessions/{session_id}/datasets — enumerate datasets currently
    /// open in a backend session. Used by cross-dataset tools (Spectral
    /// Stacking, etc.) to populate selection lists.
    BackendSessionDatasetsResult listSessionDatasets(const QString &sessionId) const;

    /**
     * Open a FITS dataset on the backend.
     * On success the returned sessionId is automatically stored in this client
     * so all subsequent calls carry the correct X-Visivo-Session header.
     */
    BackendOpenDatasetResult openDataset(const QString &path);
    BackendCatalogueInfo openCatalogue(const QString &path, const QString &format = QStringLiteral("csv"));
    BackendSampSendResult sendFitsToSamp(const QString &path,
                                         const QString &filename = QString()) const;
    BackendSampSendResult sendCatalogueToSamp(const QString &path, const QString &format,
                                              const QString &filename = QString()) const;
    BackendSampImportResult importSampUrl(const QString &url,
                                          const QString &filename = QString()) const;
    BackendSampImportResult uploadFileToSampStorage(const QString &localPath,
                                                    const QString &filename = QString()) const;

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

    // ── Binary transport (avoids base64 overhead for large arrays) ───────────
    BinaryArrayResult fetchMomentBinary(const QString &datasetId,
                                        int order,
                                        int channelStart, int channelEnd,
                                        bool maskEnabled, float threshold,
                                        const QString &sessionId);
    BinaryArrayResult fetchPvBinary(const QString &datasetId,
                                    const QVector<QPointF> &path,
                                    int slitWidth,
                                    const QString &sessionId);

    // ── S-02: Line-width maps ─────────────────────────────────────────────────
    /** POST /v1/spectral/linewidth/binary — FWHM + EW maps concatenated as two
     *  binary frames.  Parse with parseBinaryFrame() twice.
     *
     *  @param restFreqHz  Rest frequency in Hz.  Pass 0.0 (default) to omit
     *                     the field from the request — correct for velocity-axis
     *                     cubes.  Pass a positive value for frequency / wavelength
     *                     axes when velocity-unit output is required. */
    BackendLinewidthResult fetchLinewidthBinary(const QString &datasetId,
                                                int channelStart, int channelEnd,
                                                bool maskEnabled, float threshold,
                                                double restFreqHz = 0.0);

    // ── S-03: Baseline subtraction ────────────────────────────────────────────
    /** POST /v1/spectral/baseline/{session_id}/{dataset_id} — subtract a baseline
     *  and register the result as a new session dataset.
     *  @param method        "polynomial" (default) or "median"
     *  @param lineFreeRanges  channel ranges [[start,end],…] to use for the fit */
    BackendBaselineResult requestBaseline(const QString &datasetId,
                                          int channelStart, int channelEnd,
                                          int polyOrder,
                                          const QString &method = QStringLiteral("polynomial"),
                                          const QVector<QPair<int,int>> &lineFreeRanges = {});

    // ── S-04: Spectral stacking ───────────────────────────────────────────────
    /** POST /v1/spectral/stack/binary — stack N cubes into a 1-D spectrum.
     *  @param datasetIds  Dataset IDs already opened in this session.
     *  @param method      "mean", "median", or "weighted_mean".
     *  @param weights     Optional per-cube weights (used only for weighted_mean).
     *  @param weightBy    "uniform", "rms", or "peak" (auto-weights for weighted_mean). */
    BackendStackResult fetchStackBinary(const QStringList &datasetIds,
                                         const QString &method = QStringLiteral("mean"),
                                         const QVector<double> &weights = {},
                                         const QString &weightBy = QStringLiteral("uniform"));

    // ── S-05: SoFiA-2 source finding ──────────────────────────────────────────
    /** POST /v1/sourcefind/{dataset_id} — run SoFiA-2 on the cube.
     *  Returns 503 if sofia2 is not on PATH (check result.error). */
    BackendSourceFindResult runSourceFind(const QString &datasetId,
                                           const QVariantMap &sofiaParams = {});

    // ── Binary frame parsing ──────────────────────────────────────────────────
    /** Parse one Visivo binary frame starting at \c offset into \c payload.
     *  Fills \c result and advances \c offset by 12 + rows*cols*4 bytes. */
    static bool parseBinaryFrame(const QByteArray &payload, int &offset,
                                  BinaryArrayResult &result);

    // ── A-01: Aperture photometry ─────────────────────────────────────────────
    /** POST /v1/photometry/aperture/{dataset_id} */
    BackendAperturePhotometryResult requestAperturePhotometry(
            const QString &datasetId,
            double raDeg, double decDeg,
            double apertureArcsec,
            double annulusInnerArcsec,
            double annulusOuterArcsec,
            int channel = -1);

    // ── A-02: 2-D Gaussian source fitting ────────────────────────────────────
    /** POST /v1/photometry/gaussfit/{dataset_id} */
    BackendGaussFit2DResult requestGaussFit2D(
            const QString &datasetId,
            double raDeg, double decDeg,
            double boxArcsec,
            int channel = -1);

    // ── A-03: WCS reprojection ────────────────────────────────────────────────
    /** POST /v1/astrometry/reproject/{dataset_id}
     *  @param targetWcs   FITS header key→value pairs for the target WCS.
     *  @param targetShape [ny, nx] of the output grid. */
    BackendReprojectResult requestReproject(
            const QString &datasetId,
            const QVariantMap &targetWcs,
            int targetNy, int targetNx,
            int order = 1);

    // ── A-04: Catalogue cross-match ───────────────────────────────────────────
    /** GET /v1/astrometry/crossmatch/{dataset_id}
     *  @param catalogue  "simbad", "2MASS", "NVSS", or "FIRST". */
    BackendCrossmatchResult requestCrossmatch(
            const QString &datasetId,
            double radiusArcsec = 60.0,
            const QString &catalogue = QStringLiteral("simbad"));

    // ── P-01: Stokes plane extraction ─────────────────────────────────────────
    /** GET /v1/stokes/{dataset_id}/binary?plane=I&channel=0
     *  Returns the Stokes plane as a binary frame (float32 LE). */
    BackendStokesPlaneResult requestStokesPlane(
            const QString &datasetId,
            const QString &plane = QStringLiteral("I"),
            int channel = 0);

    // ── P-02: Polarised intensity and angle ───────────────────────────────────
    /** POST /v1/polarisation/pi/result/binary
     *  Returns PI and PA maps as two concatenated binary frames. */
    BackendPolarisationPiResult requestPolarisationPi(
            const QString &datasetId,
            int channel = 0,
            const QVector<int> &lineFreeChannels = {});

    // ── P-03: Rotation Measure synthesis ──────────────────────────────────────
    /** POST /v1/polarisation/rmsynthesis/{dataset_id}/binary
     *  Returns the FDF cube |F(φ)| as a binary frame (nPhi*ny rows × nx cols float32).
     *  @param phiRange  [phi_min, phi_max] in rad/m².
     *  @param dPhi      φ step in rad/m². */
    BackendRmSynthesisResult requestRmSynthesis(
            const QString &datasetId,
            double phiMin = -100.0,
            double phiMax = 100.0,
            double dPhi = 1.0);

    // ── C-01: Angular power spectrum ──────────────────────────────────────────
    /** POST /v1/cosmology/angular_power/{dataset_id} */
    BackendAngularPowerResult requestAngularPower(
            const QString &datasetId,
            int channel = 0,
            int nBins = 64);

    // ── C-03: Skeleton / void detection ───────────────────────────────────────
    /** POST /v1/analysis/skeleton/{dataset_id}/binary
     *  @param threshold  nullptr → server uses mean.
     *  @param mode       "skeleton" or "void". */
    BackendSkeletonResult requestSkeleton(
            const QString &datasetId,
            int channel = 0,
            double *threshold = nullptr,
            const QString &mode = QStringLiteral("skeleton"));

    // ── R-01: CARTA script export ─────────────────────────────────────────────
    /** POST /v1/reproducibility/carta_script
     *  @param datasetIds  Empty list = all session datasets. */
    BackendCartaExportResult requestCartaExport(
            const QStringList &datasetIds = {});

    // ── R-02: PROV-JSON provenance ────────────────────────────────────────────
    /** GET /v1/reproducibility/provenance */
    BackendProvJsonResult requestProvJson();

    // ── R-03: Parameter snapshots ─────────────────────────────────────────────
    /** GET /v1/reproducibility/snapshots — list all recorded snapshots. */
    BackendSnapshotListResult listSnapshots();
    /** POST /v1/reproducibility/snapshots — record a parameter snapshot. */
    BackendSnapshotListResult recordSnapshot(
            const QString &operation,
            const QString &datasetId = {},
            const QVariantMap &params = {});

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
