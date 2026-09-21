#ifndef BackendClient_h
#define BackendClient_h

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QMutex>
#include <QWaitCondition>
#include <QJsonArray>
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

/// What one file is, as answered by POST /v1/files/classify.
///
/// The Data Hub used to ask the user to choose the *kind* of file before the
/// file: three "Open …" entries, two of which accepted `.fits`. This result is
/// what replaced that question — the backend looks at the bytes, and the client
/// only asks when the content genuinely is ambiguous (`opener == "ask"`).
struct BackendClassifyResult
{
    bool valid{ false };
    QString error;
    QString path;
    /// The path to hand the opener when it differs from `path` — a VBT chosen
    /// by its `.bin` opens by its `.head` sidecar. Empty means "use `path`".
    QString openPath;
    /// image | cube | dynspec | fits-table | catalogue | vbt | compressed |
    /// directory | unknown
    QString kind;
    /// dataset | catalogue | vbt | ask | unsupported
    QString opener;
    /// False when the client should ask the user rather than dispatch.
    bool confident{ false };
    /// One sentence naming what was found, for the prompt when we do ask.
    QString detail;
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

/// One diagnostics event mirrored from the backend's in-process ring buffer.
/// Same shape as the Python ``DiagnosticsEvent`` schema; consumed by the
/// client poller that re-publishes events into ``DiagnosticsManager``.
struct BackendDiagnosticsEvent
{
    qint64  id{ 0 };
    double  ts{ 0. };               ///< UNIX epoch seconds
    QString level;                  ///< "debug" | "info" | "warning" | "error"
    QString category;               ///< Scientific / Backend / Task / WCS / Remote / …
    QString source;
    QString message;
    QString datasetId;
    QString sessionId;
    QString operationTag;
};

struct BackendDiagnosticsEventsResult
{
    bool valid{ false };
    QString error;
    qint64 lastId{ 0 };             ///< Highest id present in the backend buffer
    std::vector<BackendDiagnosticsEvent> events;
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
    /// Header RESTFRQ/RESTFREQ in Hz; 0 when the header carries neither. Needed
    /// to place a spectral-line list on a velocity axis (rest freq ↔ velocity).
    double restFreqHz{ 0.0 };
    /// The backend can render server-side (it probed an EGL context). False on
    /// a CPU-only or macOS backend, where "Switch to Remote" cannot work at all
    /// — /v1/render/* refuses it.
    bool serverSideRenderingAvailable{ false };
    std::array<double, 3> spacing{ 1.0, 1.0, 1.0 };
    std::array<double, 3> origin{ 0.0, 0.0, 0.0 };
    QString bunit;   ///< raw BUNIT from the FITS header (physical value unit)
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
    /// Synthesised beam (from BMAJ/BMIN/BPA FITS keywords). Values are in
    /// degrees (major/minor) and degrees E of N (PA). NaN means absent.
    double beamMajorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double beamMinorDeg{ std::numeric_limits<double>::quiet_NaN() };
    double beamPaDeg{ 0.0 };
    /// Size-aware loading policy (added for Pattern B).
    /// ``recommendedLoadMode`` is "auto_full" when the cube fits the
    /// laptop's RAM/VTK budget comfortably, or "preview_only" when the
    /// client should default to the downsampled preview and let the user
    /// opt into full-resolution or a ROI crop explicitly. Threshold is
    /// controlled by the backend (VISIVO_FULL_RES_THRESHOLD_BYTES env,
    /// default 4 GiB). For datasets older than this field (legacy
    /// backend) it defaults to "auto_full" so behaviour is unchanged.
    qint64 fileSizeBytes{ 0 };
    qint64 fullResThresholdBytes{ qint64(4) << 30 };
    QString recommendedLoadMode{ QStringLiteral("auto_full") };
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
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QString spectralAxisType;
    QString spectralAxisUnit;
    QString bunit;
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
    // Fraction (0..1) of finite pixels in the region that were exactly 0.
    // High values typically mean the region overlaps off-source blanked
    // area; the dialog surfaces a warning when this is the case.
    double blankFraction{ 0.0 };
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
    // Cartesian (3-D) catalogues (.speck / XYZ): positions are the X/Y/Z fields.
    QString xField;
    QString yField;
    QString zField;
    QString coordSystem{ QStringLiteral("sky") };   // "sky" or "cartesian"
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

struct BackendImageTileResult
{
    bool valid{ false };
    QString error;
    int width{ 0 };        ///< actual tile pixel dims (≤ tileSize at edges)
    int height{ 0 };
    int fullWidth{ 0 };
    int fullHeight{ 0 };
    int level{ 0 };
    int numLevels{ 1 };
    int tileX{ 0 };
    int tileY{ 0 };
    int tileSize{ 256 };
    QString scalarType;
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QByteArray data;       ///< float32 LE, row-major, width*height
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
    // distancesMpc carries NaN where the backend reported null — a redshift it
    // refused (negative or non-finite). NOT 0.0: that is a real distance, and the
    // catalogue viewer already treats 0.0 as "no override".
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

/// Result of POST /v1/datasets/cutout with ``save_to_workspace=true``: a
/// SKAVA SODA subset computed on the node and saved into the backend's
/// Workspace Exports directory.
struct BackendSodaCutoutResult
{
    bool valid{ false };
    QString error;
    /// Absolute path on the *backend* filesystem of the saved cutout FITS.
    QString path;
    /// Comma-separated list of subsets that took effect, e.g. "pos,band".
    QString applied;
};

/// Result of the advanced 2-D product endpoints that write a FITS into the
/// backend Workspace Exports (UC 3.2 pixel math, UC 3.3 mosaic).
struct BackendProductFitsResult
{
    bool valid{ false };
    QString error;
    QString outputPath;           ///< Backend-side path of the produced FITS
    QString workspaceFilename;    ///< Basename within Workspace Exports
    int width{ 0 };
    int height{ 0 };
    double rangeMin{ 0.0 };
    double rangeMax{ 0.0 };
};

/// Result of POST /v1/products/publication_figure (UC 1.3).
struct BackendPublicationFigureResult
{
    bool valid{ false };
    QString error;
    QString outputPath;           ///< Backend-side path of the rendered PNG/PDF/SVG
    int width{ 0 };
    int height{ 0 };
    bool hasWcs{ false };
};

/// Result of POST /v1/products/image_quality (UC 4.2). ``metrics`` carries the
/// raw metric map (blanked_fraction, dynamic_range, striping_metric, flags, …).
struct BackendImageQualityResult
{
    bool valid{ false };
    QString error;
    QVariantMap metrics;
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

// ── Full-cube statistics (computed out-of-core on the full-res cube) ───────────

struct BackendCubeStatsResult
{
    bool valid{ false };
    QString error;
    long validCount{ 0 };
    long blankedCount{ 0 };
    double min{ 0. };
    double max{ 0. };
    double mean{ 0. };
    double median{ 0. };
    double std{ 0. };
    double rms{ 0. };
    double sum{ 0. };
    bool medianExact{ false };
    QString bunit;
};

// ── Scientific copilot (R-22) ─────────────────────────────────────────────────

struct BackendAssistantToolCall
{
    QString name;
    QString argumentsJson;   // compact JSON of the arguments
    QString resultJson;      // compact JSON of the result (or empty)
    QString error;           // non-empty if the tool call failed
};

struct BackendAssistantResult
{
    bool valid{ false };
    QString error;
    QString answer;
    QString provider;             // "null" / "anthropic" / …
    bool configured{ false };     // whether an LLM provider is wired
    bool toolCallsRecorded{ false };  // ≥1 tool call traced (NOT a grounding guarantee)
    QString reproducibilityScript;
    QList<BackendAssistantToolCall> toolCalls;
    // Visualisation directives the copilot asked for (from ACTION: tools). Each
    // object carries a "type" (set_colormap / set_threshold / goto_channel / …)
    // plus its parameters; the active viewer applies them in order (reversible).
    QList<QJsonObject> actions;
};

struct BackendAssistantConfigResult
{
    bool valid{ false };
    QString error;
    bool configured{ false };
    QString provider;             // active provider after the change
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
    /// Non-fatal caveats about a fit that SUCCEEDED — chiefly a polynomial
    /// extrapolated far beyond the line-free channels, whose far end is
    /// unconstrained and can carry a large false signal. Empty when there is
    /// nothing to say. Shown to the user, never swallowed.
    QString warning;
};

// ── Sub-cube extraction (crop ROI → new FITS dataset) ─────────────────────────

struct BackendSaveSubregionResult
{
    bool valid{ false };
    QString error;
    /// ID of the newly registered dataset on the backend session — open it
    /// directly with the dataset_id-aware open flow.
    QString newDatasetId;
    /// On-disk path of the saved FITS in the backend's Workspace Exports dir.
    QString outputPath;
    /// Basename of the artefact within the Workspace Exports dir, used for
    /// the list / delete / download endpoints.
    QString workspaceFilename;
    int width { 0 };
    int height{ 0 };
    int depth { 0 };
    double rangeMin{ 0. };
    double rangeMax{ 0. };
};

// ── Dynamic-spectrum / beamformed analysis ───────────────────────────────────

struct BackendDynspecDedisperseResult
{
    bool valid{ false };
    QString error;
    QString newDatasetId;
    QString outputPath;
    QString workspaceFilename;
    int width{ 0 };
    int height{ 0 };
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    double dmPcCm3{ 0. };
    double refFreqMhz{ 0. };
    int maxShiftSamples{ 0 };
};

struct BackendDynspecPulseProfileResult
{
    bool valid{ false };
    QString error;
    QVector<double> phase;     ///< Centre of each phase bin (0…1)
    QVector<double> profile;   ///< Mean intensity per phase bin
    QVector<int>    counts;    ///< Sample count per bin (NaN bins have count=0)
    double peak{ 0. };
    double sigmaMad{ 0. };
    double snrPeak{ 0. };
    double periodS{ 0. };
    double dmPcCm3{ 0. };
    int nBins{ 0 };
    int nSamples{ 0 };
};

struct BackendDynspecRfiMaskResult
{
    bool valid{ false };
    QString error;
    QString newDatasetId;
    QString outputPath;
    QString workspaceFilename;
    int width{ 0 };
    int height{ 0 };
    double flaggedFraction{ 0. };
    double nSigma{ 0. };
    QString axis;  ///< 'channel' or 'time'
};

// ── Single-channel 2-D FITS export from a cube ───────────────────────────────

struct BackendSaveChannel2dResult
{
    bool valid{ false };
    QString error;
    /// ID of the newly registered 2-D image dataset on the backend session.
    QString newDatasetId;
    /// On-disk path of the saved 2-D FITS in the Workspace Exports dir.
    QString outputPath;
    /// Basename of the artefact within the Workspace Exports dir.
    QString workspaceFilename;
    int width{ 0 };
    int height{ 0 };
    int channel{ 0 };
    double rangeMin{ 0. };
    double rangeMax{ 0. };
};

// ── Moment map → standalone FITS export ──────────────────────────────────────

struct BackendSaveMomentFitsResult
{
    bool valid{ false };
    QString error;
    /// On-disk path of the 2-D moment FITS in the backend's Workspace Exports dir.
    QString outputPath;
    /// Basename within the Workspace Exports dir.
    QString workspaceFilename;
    int width { 0 };
    int height{ 0 };
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QString bunit;        ///< Moment unit copied into BUNIT
    QString momentUnit;   ///< Same value as bunit, kept for symmetry with /moment
};

// ── VLKB catalogue queries (TAP) ─────────────────────────────────────────────

struct BackendVlkbPreset
{
    QString id;
    QString label;
    QString table;
};

struct BackendVlkbPresetsResult
{
    bool valid{ false };
    QString error;
    QList<BackendVlkbPreset> presets;
    QStringList bands;
    QString serviceUrl;
};

struct BackendVlkbTapResult
{
    bool valid{ false };
    QString error;
    QString url;
    /// The ADQL actually run — a preset is built on the backend, so this is how
    /// the user sees what it asked for.
    QString query;
    QStringList columns;
    QList<QStringList> rows;
    int totalRows{ 0 };
    bool truncated{ false };
    QString outputPath;
    QString workspaceFilename;
    /// The service stopped at the requested row limit, so there may be more.
    bool hitRowLimit{ false };
};

// ── hips2fits and cone search ────────────────────────────────────────────────

struct BackendHips2FitsResult
{
    bool valid{ false };
    QString error;
    QString outputPath;
    QString workspaceFilename;
    qint64 bytes{ 0 };
    bool isFits{ false };
    /// The request as sent, so what was asked for is reproducible outside the app.
    QString url;
};

struct BackendHips2FitsOptions
{
    bool valid{ false };
    QString error;
    QStringList surveys;
    QStringList projections;
    QStringList stretches;
    QString endpoint;
};

struct BackendConeSearchResult
{
    bool valid{ false };
    QString error;
    QString url;
    QStringList columns;
    QList<QStringList> rows;
    int totalRows{ 0 };
    bool truncated{ false };
    /// The whole result saved as CSV, ready to open as a catalogue.
    QString outputPath;
    QString workspaceFilename;
    QString raColumn;
    QString decColumn;
};

struct BackendConeSearchService
{
    QString name;
    QString url;
};

// ── FITS header editing ──────────────────────────────────────────────────────

struct BackendHeaderCardEdit
{
    QString key;
    QString value;
    /// Only sent when `commentProvided` is true. An empty comment that IS sent
    /// means "clear this card's comment"; not sending it means "leave it as it
    /// is". Without the distinction, editing a value would wipe a comment the
    /// user never touched.
    QString comment;
    bool commentProvided{ false };
};

struct BackendHeaderPatchResult
{
    bool valid{ false };
    QString error;
    QString outputPath;
    QString workspaceFilename;
    /// One line per edit, naming the old value as well as the new.
    QStringList changes;
    /// "celestial" | "degraded" | "none" | "unknown" — what the edited header
    /// now amounts to. The legacy reported success either way.
    QString wcsStatus;
    QString wcsMessage;
    bool wcsCelestial{ false };
};

// ── Filter FITS: smooth and/or regrid a file into the workspace ──────────────

struct BackendFilterFitsResult
{
    bool valid{ false };
    QString error;
    /// On-disk path of the new FITS in the backend's Workspace Exports dir.
    QString outputPath;
    QString workspaceFilename;
    int width{ 0 };
    int height{ 0 };
    int depth{ 1 };
    double rangeMin{ 0. };
    double rangeMax{ 0. };
    QString bunit;
    /// "mean" or "sum" — how each block was combined, decided from BUNIT when
    /// the request asked for "auto".
    QString combine;
    double smoothFwhmPixels{ 0. };
    double smoothFwhmArcsec{ 0. };
    int regridFactor{ 1 };
    /// 1.0 unless the beam grew under a per-beam unit, in which case the pixels
    /// were scaled with it so the integrated flux is unchanged.
    double fluxScale{ 1. };
    /// The unit is per beam, the map was smoothed, and the header carries no
    /// beam — the values could not be rescaled and are no longer comparable.
    bool beamMissing{ false };
};

// ── Workspace Exports listing ────────────────────────────────────────────────

struct BackendWorkspaceExportEntry
{
    QString filename;       ///< basename relative to the workspace dir
    qint64  size{ 0 };      ///< bytes
    QString modifiedTime;   ///< ISO-8601 UTC timestamp
    QString absolutePath;   ///< on-disk path (diagnostic)
};

struct BackendWorkspaceExportsList
{
    bool valid{ false };
    QString error;
    QString workspaceDir;
    QVector<BackendWorkspaceExportEntry> entries;
};

struct BackendWorkspaceDeleteResult
{
    bool valid{ false };
    QString error;
    QString deletedFilename;
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
    /// Backend-side path of the SoFiA binary mask cube, when written. The
    /// desktop opens this as a 3D isosurface overlay on the HI cube.
    QString maskPath;
};

/// One channel of a spectrum extracted node-side (pixel probe or aperture).
struct BackendSpectrumResult
{
    bool valid{ false };
    QString error;
    QVector<double> spectralAxis;   ///< world values along the spectral axis
    QVector<double> values;         ///< flux per channel (NaN where blanked)
    QString spectralUnit;           ///< e.g. "Hz", "m", "pix"
    QString spectralType;           ///< CTYPE3, e.g. "FREQ", "VRAD"
};

// ── A-01: Aperture photometry ─────────────────────────────────────────────────

struct BackendAperturePhotometryResult
{
    bool valid{ false };
    QString error;
    /// NaN when the backend reports the value as unavailable (null): flux_jy /
    /// flux_err_jy need per-beam data with a known beam, the raw sum is
    /// unavailable when the aperture covers blanked pixels, and the error is
    /// unavailable when no background annulus was requested. Test with
    /// std::isnan before displaying — 0.0 would read as a measured zero.
    double fluxJy{ std::numeric_limits<double>::quiet_NaN() };
    double fluxErrJy{ std::numeric_limits<double>::quiet_NaN() };
    /// Raw background-subtracted sum in @ref fluxUnit (always in the data's own
    /// BUNIT·pixel, never relabelled Jy), and its uncertainty. NaN as above.
    double flux{ std::numeric_limits<double>::quiet_NaN() };
    double fluxErr{ std::numeric_limits<double>::quiet_NaN() };
    QString fluxUnit;
    double backgroundJyPerPix{ 0. };
    int nPixAperture{ 0 };
    /// Fractional pixel overlap area actually used for the background
    /// subtraction; @ref nPixAperture is its truncated whole-pixel count.
    double apertureAreaPix{ 0. };
    int channel{ -1 };          // -1 means "not set"
    double pixX{ 0. };
    double pixY{ 0. };
    double pixelScaleArcsec{ 0. };
};

// ── SED fitting (greybody + VLKB theoretical models) ──────────────────────────

/// One photometric point sent to the backend for a SED fit.
struct BackendSedPoint
{
    double wavelengthUm{ 0. };
    double fluxJy{ 0. };
    /// 0 when the catalogue gives no error. The backend then falls back to a
    /// Poisson-like χ² and says in its warnings that the interval is not a
    /// confidence interval.
    double errorJy{ 0. };
    bool upperLimit{ false };
};

/// What to fit and over which grid. Every range is [min, max, steps]; a range
/// with one step holds that parameter fixed at its minimum.
struct BackendSedFitRequest
{
    /// "thin", "thick" or "both".
    QString model{ QStringLiteral("thin") };
    QVector<BackendSedPoint> points;
    double distancePc{ 1000. };
    double kappaRef{ 0.1 };          ///< cm² g⁻¹ at @ref lambdaRefUm
    double lambdaRefUm{ 300. };
    bool colourCorrection{ true };   ///< PACS 70/100/160 colour corrections
    QVector<double> massRange;       ///< thin
    QVector<double> tempRange;
    QVector<double> betaRange;
    double sizeArcsec{ 0. };         ///< thick: the measured source size
    QVector<double> lambda0Range;
    QVector<double> scaleRange;
    QString sourceLabel;
};

/// One fitted model. @ref parameters and @ref uncertainties are keyed by the
/// backend's own names (mass_msun, temperature_k, beta, lambda0_um,
/// size_arcsec, luminosity_lsun, l_over_m).
struct BackendSedFit
{
    QString model;
    QMap<QString, double> parameters;
    QMap<QString, double> uncertainties;
    /// Parameters whose Δχ² = 1 interval ran off the end of the searched range:
    /// their uncertainty is a lower bound, not an interval.
    QStringList unbounded;
    /// Parameters whose best value sits on the edge of the search range.
    QStringList atBoundary;
    QStringList warnings;
    double chi2{ 0. };
    /// NaN when @ref dof is not positive: an under-determined fit has no
    /// reduced χ². Test with std::isnan before displaying or ranking on it.
    double chi2Reduced{ std::numeric_limits<double>::quiet_NaN() };
    /// N detections − N free parameters. Can be zero or negative.
    int dof{ 0 };
    int iterations{ 0 };
    bool converged{ false };
    double luminosityLsun{ 0. };
    int bandsUsed{ 0 };
    int bandsUpperLimit{ 0 };
    /// The model SED, densely sampled (5–2000 µm).
    QVector<double> modelWavelengthUm;
    QVector<double> modelFluxJy;
    /// The model evaluated at the input bands, in the order they were sent.
    QVector<double> bandWavelengthUm;
    QVector<double> fittedFluxJy;
};

struct BackendSedFitResult
{
    bool valid{ false };
    QString error;
    QVector<BackendSedFit> fits;
    /// Name of the model with the lower reduced χ² ("thin" or "thick").
    QString best;
};

/// One pre-computed theoretical model returned by the VLKB model service.
struct BackendSedModel
{
    /// Every physical column the service returned (clump_mass, dust_temp,
    /// bolometric_luminosity, chi2, …), by name.
    QMap<QString, QVariant> fields;
    double chi2{ 0. };
    QVector<double> wavelengthUm;
    QVector<double> fluxJy;
};

struct BackendSedModelsResult
{
    bool valid{ false };
    QString error;
    /// Column names in the service's own order, for the results table.
    QStringList columns;
    QVector<BackendSedModel> models;   ///< sorted by χ², best first
    int totalModels{ 0 };
    int skippedModels{ 0 };
    bool truncated{ false };
};

/// Fit ranges and constants the backend suggests, so the dialog does not carry
/// a second copy of the same numbers.
struct BackendSedDefaults
{
    bool valid{ false };
    QString error;
    QVector<double> massRange;
    QVector<double> tempRange;
    QVector<double> betaRange;
    QVector<double> lambda0Range;
    QVector<double> scaleRange;
    double kappaRef{ 0.1 };
    double lambdaRefUm{ 300. };
    double distancePc{ 1000. };
    QString modelsServiceUrl;
};

// ── VLKB multi-cutout (asynchronous batch) ───────────────────────────────────

/// One dataset of a batch request: what to cut, and where.
struct BackendMcutoutItem
{
    QString id;      ///< the archive's obs_publisher_did
    QString pos;     ///< "CIRCLE l b r" or "RANGE l1 l2 b1 b2"
};

struct BackendMcutoutSubmitResult
{
    bool valid{ false };
    QString error;
    QString jobId;
    QString phase;
    int count{ 0 };
};

struct BackendMcutoutPhaseResult
{
    bool valid{ false };
    QString error;
    QString phase;
    /// The job has stopped, one way or another: COMPLETED, ABORTED or ERROR.
    bool terminal{ false };
};

/// What became of one dataset of the batch.
struct BackendMcutoutOutcome
{
    int index{ -1 };
    bool ok{ false };
    QString filename;
    /// Whatever the archive said about a failure — the only clue to why one
    /// dataset of twenty came back empty.
    QString detail;
};

struct BackendMcutoutReportResult
{
    bool valid{ false };
    QString error;
    QVector<BackendMcutoutOutcome> results;
};

struct BackendMcutoutFetchResult
{
    bool valid{ false };
    QString error;
    QString archivePath;      ///< the .tar.gz, on the BACKEND
    QStringList files;        ///< the cutouts extracted from it
    qint64 bytes{ 0 };
};

// ── Simulation snapshots (AREPO) ──────────────────────────────────────────────

/// One node of a snapshot's HDF5 tree.
struct BackendArepoNode
{
    QString name;
    QString kind;                ///< "group" | "dataset"
    QVector<int> shape;          ///< dataset dimensions; empty for a group
    QString dtype;
    /// The node's HDF5 attributes, already reduced to displayable values.
    QList<QPair<QString, QString>> attributes;
    QVector<BackendArepoNode> children;
};

struct BackendArepoInspectResult
{
    bool valid{ false };
    QString error;
    QString path;
    BackendArepoNode tree;
    /// Side of the simulation box in kpc. NaN when the snapshot does not say.
    double boxSizeKpc{ std::numeric_limits<double>::quiet_NaN() };
    double unitLengthCm{ std::numeric_limits<double>::quiet_NaN() };
    /// Particle type ("PartType0"…) → the datasets it carries.
    QMap<QString, QStringList> particleTypes;
    /// Whether this backend can actually run an extraction (yt installed).
    bool ytAvailable{ false };
};

/// What an extraction would produce, worked out before anything is allocated.
struct BackendArepoPlan
{
    bool valid{ false };
    QString error;
    QVector<int> shape;
    QVector<double> cellSizePc;
    /// What was asked for…
    QVector<double> windowKpc;
    /// …and what will actually be extracted: the cell size is fixed by the
    /// level, so the window is rounded to a whole number of cells.
    QVector<double> extentKpc;
    QVector<double> offsetKpc;
    QVector<double> domainWidthKpc;
    int level{ 0 };
    qint64 cells{ 0 };
    qint64 bytesEstimate{ 0 };
};

struct BackendArepoCube
{
    QString field;
    QString particle;
    QString path;      ///< on the BACKEND's filesystem
    QString bunit;
    QVector<int> shape;
};

struct BackendArepoExtractResult
{
    bool valid{ false };
    QString error;
    BackendArepoPlan plan;
    QVector<BackendArepoCube> outputs;
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
    double deconvolvedPaDeg{ std::numeric_limits<double>::quiet_NaN() };
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

// ── R-01: CARTA script export — RETIRED (feature removed). ────────────────────

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

// ── Region interchange (CRTF / DS9) ───────────────────────────────────────────

struct BackendRegionParseResult
{
    bool valid{ false };
    QString error;
    QString format;               ///< detected/used format: "crtf" | "ds9"
    /// Canonical region dicts as returned by the backend (shape/frame/center/…).
    /// Kept as raw JSON so the viewer can map them onto its overlay model.
    QJsonArray regions;
};

struct BackendRegionExportResult
{
    bool valid{ false };
    QString error;
    QString text;                 ///< serialized CRTF / DS9 region file content
};

struct BackendRegionStatsResult
{
    bool valid{ false };
    QString error;
    int validCount{ 0 };
    int blankedCount{ 0 };
    double mean{ 0.0 };
    double rms{ 0.0 };
    double sum{ 0.0 };
    double minValue{ 0.0 };
    double maxValue{ 0.0 };
    double median{ 0.0 };
    double madSigma{ 0.0 };
    bool hasFlux{ false };
    double fluxJy{ 0.0 };         ///< valid only when hasFlux (Jy/beam + beam)
    QString bunit;
};

struct BackendLineFitResult
{
    bool valid{ false };
    QString error;
    double peak{ 0.0 };
    double center{ 0.0 };
    double sigma{ 0.0 };
    double fwhm{ 0.0 };
    double integratedFlux{ 0.0 };
    double continuum{ 0.0 };
    double rms{ 0.0 };
    double baselineIntercept{ 0.0 };  ///< baseline a in (a + b·x)
    double baselineSlope{ 0.0 };      ///< baseline b in (a + b·x)
    // 1-sigma fit uncertainties. Negative = the backend could not determine one
    // (a perfect fit gives an infinite covariance), so a caller must check before
    // printing "±". These are inflated for spectrally correlated channels; see
    // spectralCorrelationFactor.
    double peakErr{ -1.0 };
    double centerErr{ -1.0 };
    double sigmaErr{ -1.0 };
    double fwhmErr{ -1.0 };
    double integratedFluxErr{ -1.0 };
    /// Effective-sample factor F the errors were inflated by (√F on each error).
    /// 1 = measured, channels independent. 0 = the backend did not report it at
    /// all (older than the correction), so the errors are UNCORRECTED — a
    /// different situation, and not one to render as "no correlation".
    double spectralCorrelationFactor{ 0.0 };
    /// Set when F is large enough to matter: the same signature comes from
    /// correlated channels OR from a model that does not fit, and the backend
    /// cannot tell them apart. Shown to the user rather than swallowed.
    QString spectralCorrelationNote;
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

/// Result of opening a velocity (cosmic-flow) field — /v1/velocity/open.
struct BackendVelocityOpenResult
{
    bool valid = false;
    QString error;
    QString datasetId;
    QString sessionId;
    int nx = 0, ny = 0, nz = 0;
    double speedMin = 0.0, speedMax = 0.0;
    QString unit;
    QString frame;
    bool boxKnown = false;
};

/// Raw vector grid for a velocity field — /v1/velocity/grid_bin.
/// `vectors` is interleaved little-endian float32 (npts*3, x-fastest =
/// vtkImageData point order): vx,vy,vz per point.
struct BackendVelocityGridResult
{
    bool valid = false;
    QString error;
    int nx = 0, ny = 0, nz = 0;
    double sx = 1.0, sy = 1.0, sz = 1.0;   // spacing
    double ox = 0.0, oy = 0.0, oz = 0.0;   // origin
    double speedMin = 0.0, speedMax = 0.0;        // this (possibly LOD) grid's range
    double fullSpeedMin = 0.0, fullSpeedMax = 0.0; // full-resolution field range (for LUT/scale)
    QString unit;
    QString frame;
    int downsampleFactor = 1;              // 1 = full res; >1 = LOD block-average factor
    int fullNx = 0, fullNy = 0, fullNz = 0; // full-resolution grid dims (pre-LOD)
    int roiX0 = 0, roiY0 = 0, roiZ0 = 0;   // sub-grid start (subgrid_bin only)
    QByteArray vectors;
};

/// Tilted-ring kinematic model cube — /v1/cube/kinematic_model_bin (F2).
/// `data` is little-endian float32 (width*height*depth, x-fastest = vtkImageData
/// point order), the model emission of a regularly rotating disk to be contoured
/// over the observed cube. `spectralMode` is "velocity" | "native" | "channel"
/// and tells the GUI what unit Vsys/Vrot/sigma are in (`spectralAxisUnit`).
struct BackendKinematicModelResult
{
    bool valid = false;
    QString error;
    int width = 0, height = 0, depth = 0;
    double peak = 0.0;
    QString spectralAxisUnit;
    QString spectralMode;
    QByteArray data;
};

/// Kinematic Lasso selection — /v1/cube/kinematic_select_bin. `data` is the
/// selection's bounding-box binary mask (little-endian float32 0/1, x-fastest =
/// vtkImageData order), of size roiW×roiH×roiD, placed at (originX,originY,
/// originZ) in the full cube's voxel space. `count` = selected voxels.
struct BackendKinematicSelectResult
{
    bool valid = false;
    QString error;
    int roiWidth = 0, roiHeight = 0, roiDepth = 0;
    int originX = 0, originY = 0, originZ = 0;
    int fullWidth = 0, fullHeight = 0, fullDepth = 0;
    int count = 0;
    int bbox[6] = { 0, 0, 0, 0, 0, 0 };   // x0,x1,y0,y1,z0,z1
    QByteArray data;
};

/// Geodesic LEVEL FIELDS for the Kinematic Lasso's Reach slider —
/// /v1/cube/kinematic_levels_bin.
///
/// One grow up to ``reachMax`` instead of one per slider step: ``addLevels`` and
/// ``subLevels`` are uint8 over the same ROI, and the selection at an integer
/// budget r is ``add <= r`` minus ``sub <= r``. That threshold is EXACT, not an
/// approximation of the per-step grow — levels are ceil(distance), and for
/// integer r, ceil(d) <= r ⟺ d <= r — so the preview cannot drift from what
/// Apply writes out, provided Apply passes the same ``reachMax``.
struct BackendKinematicLevelsResult
{
    bool valid = false;
    QString error;
    int roiWidth = 0, roiHeight = 0, roiDepth = 0;
    int originX = 0, originY = 0, originZ = 0;
    int fullWidth = 0, fullHeight = 0, fullDepth = 0;
    int reachMax = 0;              ///< highest budget this field can answer for
    int unreachable = 255;         ///< level meaning "beyond the budget"
    QByteArray addLevels;          ///< uint8, roiW*roiH*roiD, x-fastest
    QByteArray subLevels;          ///< uint8, same geometry (all-sentinel if no carve)
};

/// Gravitational-basin (watershed) label volume — /v1/velocity/basins_bin.
/// `labels` is little-endian int32 (npts, x-fastest): 0 = unassigned, 1..nBasins.
struct BackendVelocityBasinsResult
{
    bool valid = false;
    QString error;
    int nx = 0, ny = 0, nz = 0;
    int nBasins = 0;
    QByteArray labels;
};

/// Density-from-divergence (∇·v) scalar grid: ∇·v < 0 = overdensity (convergent),
/// > 0 = void (divergent). Full-resolution alternative to the client's LOD-grid
/// divergence, for large fields the display LOD can't resolve.
struct BackendVelocityDensityResult
{
    bool valid = false;
    QString error;
    int nx = 0, ny = 0, nz = 0;
    double spacing[3] = { 1.0, 1.0, 1.0 };
    double origin[3] = { 0.0, 0.0, 0.0 };
    double divMin = 0.0, divMax = 0.0;
    QString unit;
    QByteArray divergence;   // little-endian float32, x-fastest (vtkImageData order)
};

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

    /**
     * Process-wide default session id, sent as X-Visivo-Session whenever a client
     * has no per-instance session id of its own. Setting this once at startup to a
     * unique value scopes ALL of this desktop's datasets (and its copilot) to a
     * private backend session — so several desktops sharing one backend don't see
     * each other's data. Empty (the default) means the backend's anonymous session.
     */
    static void setDefaultSessionId(const QString &sessionId);
    static QString defaultSessionId();

    /**
     * Ask this backend to MINT a session for us (POST /v1/sessions/new) and
     * remember it for that base URL. Server-minted ids are required by a backend
     * running with VISIVO_STRICT_SESSIONS (the default once per-user tokens are
     * configured), which refuses to adopt an id the client invented.
     *
     * Idempotent per base URL and best-effort: if the endpoint is missing (older
     * backend) or unreachable we keep using the locally generated default id, so
     * a single-user backend behaves exactly as before. Each backend gets its OWN
     * minted id — a shared default can't work once ids come from the server.
     */
    /// Outcome of ensureSessionMinted(): the backend gave us a session, the
    /// backend has no such route (keep the local default id), or we could not
    /// reach it this time (a caller building a request by hand should not send
    /// yet — a strict backend would reject it).
    enum class SessionMint { Ready, Unsupported, Deferred };
    SessionMint ensureSessionMinted() const;

    /// The session id to send to @p baseUrl: the id that backend minted for us if
    /// there is one, else the process-wide local default. Use this when building a
    /// request by hand (e.g. the copilot's SSE stream) instead of BackendClient.
    static QString sessionIdForUrl(const QString &baseUrl, const QString &token);

    /// True once @p baseUrl has minted a session for @p token (or we know it never
    /// will, because that backend has no such route). Lets a caller that builds a
    /// request by hand decide whether it must run ensureSessionMinted() first —
    /// off the GUI thread, since that performs a network round-trip.
    static bool sessionResolvedForUrl(const QString &baseUrl, const QString &token);
    /// Blocking convenience for hand-built requests: ensure a session and report
    /// the identity that actually authenticated. ``effectiveToken`` / ``sessionId``
    /// are what such a caller must put on the wire — the mint may have adopted a
    /// refreshed token, in which case the caller's original one is stale.
    /// The token an alias-aware lookup would actually authenticate with: follows
    /// a rotation the mint recorded (old token → refreshed one) for *baseUrl*.
    /// applySessionHeaders() does this internally; call it directly only when the
    /// token goes somewhere that is not a request header — a WebSocket query
    /// string, say — where the stale one would fail auth on its own.
    static QString effectiveToken(const QString &baseUrl, const QString &token);

    /// Stamp the auth + session headers on a request built by hand — the direct
    /// QNetworkAccessManager paths (remote render, WebRTC signalling, SAMP) that
    /// do not go through buildRequest(). Without the session header those land in
    /// a DIFFERENT backend session than the rest of the client's traffic, which
    /// under multi-user mode means they cannot see the client's datasets at all.
    /// Pass ``sessionOverride`` when the caller already holds a session id.
    static void applySessionHeaders(QNetworkRequest &request, const QString &baseUrl,
                                    const QString &token,
                                    const QString &sessionOverride = QString());

    static SessionMint ensureSessionFor(const QString &baseUrl, const QString &token,
                                        QString *effectiveToken = nullptr,
                                        QString *sessionId = nullptr);

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

    /// POST /v1/files/classify — what is this file? Feeds the single "Open…".
    BackendClassifyResult classifyFile(const QString &path) const;
    BackendFileHeaderResult fileHeader(const QString &path) const;

    /// GET /v1/sessions/{session_id}/datasets — enumerate datasets currently
    /// open in a backend session. Used by cross-dataset tools (Spectral
    /// Stacking, etc.) to populate selection lists.
    BackendSessionDatasetsResult listSessionDatasets(const QString &sessionId) const;

    /// GET /v1/diagnostics/events?since=N — pull new entries from the
    /// backend's in-process diagnostics ring buffer. Designed for periodic
    /// polling from MainWindow so the client can re-publish backend log
    /// records into its own DiagnosticsManager.
    BackendDiagnosticsEventsResult pollBackendDiagnostics(qint64 sinceId,
                                                          int limit = 500) const;

    /**
     * Open a FITS dataset on the backend.
     * On success the returned sessionId is automatically stored in this client
     * so all subsequent calls carry the correct X-Visivo-Session header.
     */
    BackendOpenDatasetResult openDataset(const QString &path);

    /** POST /v1/datasets/open_url — download a FITS / HDF5 from an HTTP(S)
     *  URL, cache it under the backend's exports/skava_cache directory and
     *  open it as a session dataset. Used by the SKAVA discovery flow:
     *  the desktop resolves the access URL via SKAVA's DataLink, then hands
     *  it to this endpoint. ``obsId`` is optional and ends up as a hint in
     *  the cache filename. ``remoteBearerToken`` is forwarded as a
     *  ``Authorization`` header on the download, for storage nodes that
     *  require authentication. */
    BackendOpenDatasetResult openDatasetFromUrl(const QString &url,
                                                  const QString &obsId = {},
                                                  const QString &remoteBearerToken = {});

    /** POST /v1/datasets/open_skava — open a dataset whose access URL the
     *  desktop got from SKAVA. Compute-next-to-data, no file transfer.
     *
     *  The desktop client has already done the discovery round-trip
     *  with SKAVA and knows the access URL (typically ``file://path``
     *  pointing at the backend's own filesystem). It hands BOTH the
     *  obs_id (for logging / session bookkeeping) AND the access URL to
     *  the backend, saving the backend a redundant query back to SKAVA.
     *
     *  Backend prerequisites:
     *  - access_url scheme must be ``file://``
     *  - if ``VISIVO_DATA_ROOT`` is set, the resolved path must live
     *    inside that root (jail)
     *
     *  Used by ``MainWindow::doOpenSkavaDataset`` when the routing has
     *  matched a remote backend. */
    BackendOpenDatasetResult openSkavaDataset(const QString &obsId,
                                                const QString &accessUrl);
    BackendCatalogueInfo openCatalogue(const QString &path, const QString &format = QStringLiteral("csv"));
    /// Open SEVERAL catalogue files of the same format as ONE concatenated
    /// catalogue (e.g. the spatial subtables of a cosmological snapshot).
    BackendCatalogueInfo openCatalogue(const QStringList &paths,
                                       const QString &format = QStringLiteral("csv"));
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

    /// Open a velocity (cosmic-flow) field FITS (multi-component, NAXIS4=3).
    /// Stores the returned session id (like openDataset) so the follow-up
    /// grid fetch lands in the same backend session.
    BackendVelocityOpenResult openVelocityField(const QString &path, double boxMpc = 0.0);
    /// Fetch the vector grid for a previously-opened velocity field.
    /// `maxDim` caps the display resolution (backend block-averages so the
    /// largest dimension is <= maxDim; 0 = full resolution) — keeps transfer +
    /// client memory bounded for large fields.
    BackendVelocityGridResult fetchVelocityGrid(const QString &datasetId,
                                                int maxDim = 128) const;
    /// Fetch a FULL-resolution sub-region (ROI on demand) of a velocity field,
    /// by full-res grid indices. Result carries the clamped start (roiX0/Y0/Z0).
    BackendVelocityGridResult fetchVelocitySubgrid(const QString &datasetId,
                                                   int x0, int x1, int y0, int y1,
                                                   int z0, int z1) const;
    /// Gravitational-basin (watershed) segmentation label volume. `direction`:
    /// +1 = basins of attraction (superclusters), -1 = repulsion (voids).
    BackendVelocityBasinsResult fetchVelocityBasins(const QString &datasetId,
                                                    int basinMaxDim = 64,
                                                    int direction = 1) const;
    /// Density-from-divergence (∇·v) scalar grid. `maxDim`: 0 = full resolution,
    /// >0 block-averages first (LOD). ∇·v < 0 = overdensity, > 0 = void.
    BackendVelocityDensityResult fetchVelocityDensity(const QString &datasetId,
                                                      int maxDim = 0) const;
    /// Tilted-ring kinematic model cube (F2). Builds a regularly-rotating-disk
    /// model on the cube's own grid; Vsys/Vrot/sigma are in the cube's spectral
    /// unit (km/s when the axis converts — see result.spectralMode).
    /// @param rings optional per-ring (warp / rotation-curve) profile: each entry
    /// is {r_pix, pa_deg, incl_deg, vrot}. When non-empty the backend builds a
    /// warped tilted-ring model and ignores the scalar pa/incl/vrot/rmax.
    BackendKinematicModelResult requestKinematicModel(const QString &datasetId,
                                                      double x0, double y0,
                                                      double paDeg, double inclDeg,
                                                      double vsys, double vrot,
                                                      double rmaxPix, double sigma,
                                                      const QVector<std::array<double, 4>> &rings = {}) const;

    /** POST /v1/cube/kinematic_select_bin — Kinematic Lasso. Grows a geodesic,
     *  kinematics-aware selection from the seed voxel (z,y,x) and returns the
     *  selection's bounding-box binary mask + its origin. @p reach is the cost
     *  budget, @p lambdaV the velocity-axis weight (the kinematic knob). */
    /// Fetch the geodesic level fields once so the Reach slider can be scrubbed
    /// without a round trip per step. ``reachMax`` should be the slider's maximum.
    BackendKinematicLevelsResult kinematicLevels(const QString &datasetId,
                                                 int seedZ, int seedY, int seedX,
                                                 double reachMax, double lambdaV,
                                                 double sigma, double nsigma, int roiRadius,
                                                 const QVector<std::array<int, 3>> &addSeeds,
                                                 const QVector<std::array<int, 3>> &subSeeds) const;

    BackendKinematicSelectResult kinematicSelect(const QString &datasetId,
                                                 int seedZ, int seedY, int seedX,
                                                 double reach, double lambdaV,
                                                 double sigma = 0.0, double nsigma = 2.5,
                                                 int roiRadius = 0,   // 0 = object-bounded (whole connected source)
                                                 const QVector<std::array<int, 3>> &addSeeds = {},
                                                 const QVector<std::array<int, 3>> &subSeeds = {}) const;

    /** POST /v1/cube/mask_selection — apply a Kinematic Lasso selection: blank
     *  inside (@p mode "blank_inside", remove) or outside ("blank_outside",
     *  isolate) the selection, keeping full dims + WCS, and register the masked
     *  cube as a new dataset. Reuses the save-subregion result shape. */
    BackendSaveSubregionResult maskCubeSelection(const QString &datasetId,
                                                 int seedZ, int seedY, int seedX,
                                                 const QVector<std::array<int, 3>> &addSeeds,
                                                 const QVector<std::array<int, 3>> &subSeeds,
                                                 double reach, double lambdaV, const QString &mode,
                                                 double sigma = 0.0, double nsigma = 2.5,
                                                 int roiRadius = 0,   // 0 = object-bounded (whole connected source)
                                                 const QString &outputBasename = {}) const;

    /** POST /v1/cube/mask_apply_bin — apply an EXPLICIT ROI mask (raw little-endian
     *  float32 0/1, x-fastest = the select payload) at @p origin, blanking inside
     *  (@p mode "blank_inside") or outside ("blank_outside"), and register the
     *  masked cube as a new dataset. Unlike maskCubeSelection this applies the mask
     *  bytes verbatim (no re-grow), so a hand-refined mask is applied as shown. */
    BackendSaveSubregionResult applyCubeMask(const QString &datasetId,
                                             const QByteArray &maskBytes,
                                             int originX, int originY, int originZ,
                                             int width, int height, int depth,
                                             const QString &mode,
                                             const QString &outputBasename = {}) const;
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

    /** POST /v1/datasets/cutout – SKAVA SODA subset computed next to the data.
     *  The backend opens the node-local ``file://`` dataset in place, applies
     *  the POS/BAND subset, writes the result into its Workspace Exports dir
     *  (``save_to_workspace=true``) and returns the backend-side path. The
     *  desktop then opens that path in place — the cutout never transits the
     *  client. Distinct from ``vlkbFetchCutout`` (VLKB-specific SODA URL fetch),
     *  which is intentionally kept alongside this one.
     *  @param obsId      SKAVA obs_id (bookkeeping / filename hint).
     *  @param accessUrl  The ``file://`` access URL resolved from DataLink.
     *  @param pos        SODA POS, e.g. ``CIRCLE ra dec radius`` (deg); empty to omit.
     *  @param band       SODA BAND ``"min max"`` in metres; empty to omit.
     *  @param time       SODA TIME ``"min max"`` in MJD; empty to omit. */
    BackendSodaCutoutResult sodaCutout(const QString &obsId,
                                        const QString &accessUrl,
                                        const QString &pos = {},
                                        const QString &band = {},
                                        const QString &time = {}) const;

    // ── Region interchange (CRTF / DS9) ───────────────────────────────────────
    /** POST /v1/regions/parse — parse CRTF/DS9 text into canonical regions.
     *  @param text     region file content.
     *  @param format   "auto" | "crtf" | "ds9".
     *  @param datasetId / toFrame  optional WCS reframe ("image" or a world
     *                   frame); both required together to convert. */
    BackendRegionParseResult parseRegions(const QString &text,
                                          const QString &format = QStringLiteral("auto"),
                                          const QString &datasetId = {},
                                          const QString &toFrame = {}) const;

    /** POST /v1/regions/export — serialize canonical regions to CRTF/DS9 text.
     *  @param regions  canonical region dicts (as returned by parseRegions).
     *  @param format   "crtf" | "ds9". */
    BackendRegionExportResult exportRegions(const QJsonArray &regions,
                                            const QString &format,
                                            const QString &datasetId = {},
                                            const QString &toFrame = {}) const;

    /** POST /v1/regions/stats/{dataset_id} — pixel statistics inside a region.
     *  @param region   canonical region dict (image or world frame).
     *  @param channel  cube channel (<0 → backend default 0). */
    BackendRegionStatsResult regionStats(const QString &datasetId,
                                         const QJsonObject &region,
                                         int channel = -1) const;

    /** POST /v1/spectral/linefit — Gaussian fit of an extracted 1-D spectrum. */
    BackendLineFitResult fitSpectralLine(const QVector<double> &values,
                                         const QVector<double> &axis,
                                         bool withBaseline = true) const;

    /** POST /v1/products/map_math — pixel-wise math on one/two 2-D maps
     *  (UC 3.2). ``op`` ∈ {add,subtract,multiply,divide,ratio,log10,abs,
     *  square,spectral_index}. Binary ops need ``datasetIdB``;
     *  ``spectral_index`` also needs ``freqA``/``freqB`` (Hz). */
    BackendProductFitsResult mapMath(const QString &datasetId, const QString &op,
                                     const QString &datasetIdB = {},
                                     double freqA = 0.0, double freqB = 0.0) const;

    /** POST /v1/products/mosaic — noise-weighted reproject + co-add of 2-D
     *  images (UC 3.3). ``weightMode`` ∈ {"noise","uniform"}. */
    BackendProductFitsResult mosaic(const QStringList &datasetIds,
                                    const QString &weightMode = QStringLiteral("noise")) const;

    /** POST /v1/products/publication_figure — WCS-gridded, colorbar-labelled
     *  figure (UC 1.3). ``stretch`` ∈ {linear,log,sqrt,asinh,power}. */
    BackendPublicationFigureResult publicationFigure(
            const QString &datasetId, const QString &stretch, const QString &title,
            bool addGrid, bool addColorbar,
            const QString &cmap = QStringLiteral("inferno"), int dpi = 200) const;

    /** POST /v1/products/image_quality — quality/artifact metrics (UC 4.2). */
    BackendImageQualityResult imageQuality(const QString &datasetId) const;

    BackendImageResult requestImagePreview(const QString &datasetId, int maxLongestSide) const;
    BackendImageResult requestImage(const QString &datasetId) const;
    /// One out-of-core pyramid tile (POST /v1/image/tile). level 0 = full res,
    /// L decimates by 2^L. Used to assemble large mosaics tile-by-tile so the
    /// backend never loads the whole plane. See docs/backend-api.md.
    BackendImageTileResult requestImageTile(const QString &datasetId, int level,
                                            int tileX, int tileY, int tileSize = 256) const;
    BackendIsosurfaceResult requestIsosurface(const QString &datasetId, double threshold) const;
    BackendMomentResult requestMoment(const QString &datasetId, int order, int channelStart,
                                      int channelEnd, bool maskEnabled,
                                      double thresholdValue, bool thresholdAuto = false) const;
    BackendTaskCreateResult createMomentTask(const QString &datasetId, int order, int channelStart,
                                             int channelEnd, bool maskEnabled,
                                             double thresholdValue, bool thresholdAuto = false) const;
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

    /// Short operation tag for the Diagnostics "Operation" column, derived from a
    /// request path: "/v1/spectral/linewidth/binary" → "spectral/linewidth".
    /// Public + static because it is pure string logic and unit-tested.
    static QString operationTagForPath(const QString &path);

    /// How many HEAVY compute requests this client currently has in flight.
    ///
    /// The status bar's "N running" came only from the backend's task registry,
    /// which holds the ASYNC tasks (moment, PV) — so it read "0 running" for the
    /// whole of a line-width map, an isosurface or a stack, all of which are
    /// synchronous POSTs (user report). The client knows about its own requests,
    /// so it counts them: heavy is decided by the same path list that gives those
    /// endpoints their long timeout.
    static int heavyRequestsInFlight();
    /// Parse a /v1/image/tile JSON object into a tile result (metadata only;
    /// data_base64 must still be decoded via decodePayload by the caller).
    static BackendImageTileResult parseImageTileObject(const QJsonObject &object);

    /// JSON-only parsers for the Workspace Exports lifecycle, factored out
    /// so they can be unit-tested without a live HTTP server (mirrors the
    /// pattern used by parseNoiseResultObject above).
    static BackendSaveSubregionResult       parseSaveSubregionObject(const QJsonObject &object);
    static BackendSaveChannel2dResult       parseSaveChannel2dObject(const QJsonObject &object);
    static BackendSaveMomentFitsResult      parseSaveMomentFitsObject(const QJsonObject &object);
    static BackendWorkspaceExportsList      parseExportsListObject(const QJsonObject &object);
    static BackendWorkspaceDeleteResult     parseExportsDeleteObject(const QJsonObject &object);

    // ── Binary transport (avoids base64 overhead for large arrays) ───────────
    BinaryArrayResult fetchMomentBinary(const QString &datasetId,
                                        int order,
                                        int channelStart, int channelEnd,
                                        bool maskEnabled, float threshold,
                                        const QString &sessionId,
                                        bool thresholdAuto = false);
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
                                                double restFreqHz = 0.0,
                                                /// True → the backend's vectorised path
                                                /// (moment-2 dispersion for FWHM) instead
                                                /// of one scipy curve_fit per pixel. The
                                                /// endpoint has always accepted it; the
                                                /// desktop never sent it, so every map
                                                /// paid for ~10⁶ fits (audit).
                                                bool fast = false);

    /** GET /v1/analysis/cube-stats/{dataset_id} — whole-cube statistics (min/max/mean/median/
     *  std/rms/sum + counts) computed on the FULL-resolution cube out-of-core. The desktop
     *  client only holds a decimated preview, so its own 3-D stats are approximate; this is
     *  the authoritative set. Cached backend-side per file. */
    BackendCubeStatsResult fetchCubeStats(const QString &datasetId);

    /** POST /v1/assistant/query — ask the scientific copilot a natural-language
     *  question. Every quantitative claim is backed by a recorded tool call
     *  (provenance) and a reproducibility script. When no LLM provider is wired
     *  server-side the answer explains how to enable one; ``configured`` is then
     *  false but the call still succeeds. ``datasetId`` may be empty. */
    BackendAssistantResult assistantQuery(const QString &question,
                                          const QString &datasetId,
                                          const QJsonArray &history = {});

    /** Parse a /v1/assistant/query (or the streamed ``result`` event) JSON body
     *  into a BackendAssistantResult. Shared by the blocking and streaming
     *  paths so both interpret the answer/tool-calls/actions identically. */
    static BackendAssistantResult parseAssistantResult(const QJsonObject &o);

    /** POST /v1/assistant/config — choose the copilot LLM back-end: Claude
     *  (anthropicKey), ChatGPT/OpenAI (openaiKey), or your own server
     *  (baseUrl of an OpenAI-compatible endpoint). Empty fields are left
     *  unchanged server-side; keys are held in server memory only. */
    BackendAssistantConfigResult setAssistantConfig(
            const QString &provider, const QString &anthropicKey,
            const QString &openaiKey, const QString &baseUrl,
            const QString &baseUrlKey, const QString &model);

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

    // ── Sub-cube extraction (crop ROI → Workspace Exports) ────────────────────
    /** POST /v1/cube/save_subregion — write a cropped FITS sub-cube into
     *  the persistent Workspace Exports dir and register it as a new
     *  dataset in the current session. WCS is preserved (CRPIX shifted).
     *  Inclusive 0-indexed bounds. @p outputBasename is optional; an
     *  empty value lets the backend synthesise a name from source+range. */
    BackendSaveSubregionResult saveCubeSubregion(const QString &datasetId,
                                                  int x0, int x1,
                                                  int y0, int y1,
                                                  int z0, int z1,
                                                  const QString &outputBasename = {});

    /** POST /v1/cube/mask — blank (NaN) the voxels inside or outside a 3-D box,
     *  keeping FULL cube dimensions + WCS, and register the masked cube as a new
     *  dataset in the current session. @p mode is "blank_inside" (remove the box)
     *  or "blank_outside" (isolate the box). Inclusive 0-indexed bounds. Reuses
     *  the save-subregion result shape (same fields). */
    BackendSaveSubregionResult maskCubeRegion(const QString &datasetId,
                                              int x0, int x1,
                                              int y0, int y1,
                                              int z0, int z1,
                                              const QString &mode,
                                              const QString &outputBasename = {});

    /** POST /v1/cube/save_channel_2d — extract a single channel of the cube
     *  and write it as a **standalone 2-D FITS image** (NAXIS=2, no
     *  degenerate spectral axis). The result is registered as a new ``image``
     *  dataset on the backend session and lands in the Workspace Exports dir.
     *  Used by the Stokes / spectral-index / Faraday-RM workflows when you
     *  need several single-frequency 2-D maps from one or more cubes. */
    BackendSaveChannel2dResult saveCubeChannelAs2dFits(const QString &datasetId,
                                                        int channel,
                                                        const QString &outputBasename = {});

    // ── Dynamic-spectrum analysis ────────────────────────────────────────────
    /** POST /v1/dynspec/dedisperse — incoherent dedispersion at the given
     *  DM (pc cm⁻³); writes a new 2-D dynspec FITS to the workspace and
     *  registers it as a new session dataset. ``refFreqMhz=0`` lets the
     *  backend pick the top of the band. */
    BackendDynspecDedisperseResult dedisperseDynspec(const QString &datasetId,
                                                      double dmPcCm3,
                                                      double refFreqMhz = 0.0,
                                                      const QString &outputBasename = {});

    /** POST /v1/dynspec/pulse_profile — fold the frequency-averaged time
     *  series at @p periodS and return the phase profile as in-memory
     *  arrays (no file written). ``dmPcCm3>0`` triggers in-memory
     *  dedispersion before folding. Frequency band selection via
     *  ``freqStartMhz/freqEndMhz`` (set 0 for the full band). */
    BackendDynspecPulseProfileResult pulseProfile(const QString &datasetId,
                                                   double periodS,
                                                   int nBins = 64,
                                                   double dmPcCm3 = 0.0,
                                                   double freqStartMhz = 0.0,
                                                   double freqEndMhz = 0.0);

    /** POST /v1/dynspec/rfi_mask — generate a σ-clipped RFI mask. ``axis``
     *  is either ``"channel"`` (clip across time, per channel) or
     *  ``"time"`` (clip across frequency, per time sample). Result is a
     *  uint8 FITS in the workspace registered as a new ``dynspec``
     *  dataset that can be opened as a layer over the source. */
    BackendDynspecRfiMaskResult rfiMask(const QString &datasetId,
                                         double nSigma = 5.0,
                                         const QString &axis = QStringLiteral("channel"),
                                         const QString &outputBasename = {});

    // ── Moment map → Workspace Exports ────────────────────────────────────────
    /** POST /v1/products/moment_fits — compute the requested moment and
     *  persist it as a 2-D FITS file in the Workspace Exports dir
     *  (celestial WCS + BUNIT). @p outputBasename optional; default is
     *  synthesised from source + order. */
    /// POST /v1/image/filter — Gaussian smoothing and/or an integer regrid of a
    /// FITS file, written as a new file in the workspace. Either step may be
    /// skipped (FWHM 0, factor 1), but not both.
    /// GET /v1/vlkb/tap/presets — the named catalogue queries.
    BackendVlkbPresetsResult vlkbTapPresets() const;

    /// POST /v1/vlkb/tap — run a preset over a Galactic box, or an ADQL
    /// statement, and save the result as a catalogue.
    BackendVlkbTapResult vlkbTapQuery(const QString &preset, const QString &query,
                                      double glonMin, double glonMax, double glatMin,
                                      double glatMax, const QString &band, int maxRows,
                                      const QString &outputBasename = QString()) const;

    /// GET /v1/hips/hips2fits/surveys — the starting survey list and the
    /// enumerations the service accepts.
    BackendHips2FitsOptions hips2fitsOptions() const;

    /// POST /v1/hips/hips2fits — cut a FITS (or a picture) out of a HiPS survey
    /// into the workspace.
    BackendHips2FitsResult hips2fits(const QVariantMap &parameters) const;

    /// GET /v1/resolve/cone_search/services — a few known endpoints.
    QList<BackendConeSearchService> coneSearchServices() const;

    /// POST /v1/resolve/cone_search — a VO Simple Cone Search, saved as CSV.
    BackendConeSearchResult coneSearch(const QString &serviceUrl, double ra, double dec,
                                       double radiusDeg, int verbosity,
                                       const QString &outputBasename = QString()) const;

    /// POST /v1/files/header/patch — apply header edits to a COPY of the file
    /// in the workspace, and report what the result's WCS amounts to.
    BackendHeaderPatchResult patchFitsHeader(const QString &path,
                                             const QList<BackendHeaderCardEdit> &cards,
                                             const QStringList &remove,
                                             const QString &outputBasename = QString());

    BackendFilterFitsResult filterFits(const QString &path, double smoothFwhmPixels,
                                       int regridFactor, const QString &combine,
                                       const QString &outputBasename = QString());

    BackendSaveMomentFitsResult saveMomentFits(const QString &datasetId,
                                                int order,
                                                int channelStart, int channelEnd,
                                                bool maskEnabled,
                                                double thresholdValue,
                                                const QString &outputBasename = {},
                                                bool thresholdAuto = false);

    // ── Workspace Exports management ──────────────────────────────────────────
    /** POST /v1/exports/copy_dataset — copy a dataset's FITS file into
     *  the Workspace Exports dir. Useful for 2-D images. */
    BackendWorkspaceDeleteResult copyDatasetToWorkspace(const QString &datasetId,
                                                         const QString &basename);
    /** GET /v1/exports/list — enumerate the workspace contents, newest first. */
    BackendWorkspaceExportsList listExports();
    /** DELETE /v1/exports/{filename} — remove a workspace artefact. */
    BackendWorkspaceDeleteResult deleteExport(const QString &filename);
    /** GET /v1/exports/download?filename=… — stream an artefact back and
     *  write it to @p localPath. Returns true on success; @p error is
     *  populated on failure. Synchronous (callers typically run it
     *  inside a QtConcurrent task). */
    bool downloadExport(const QString &filename,
                        const QString &localPath,
                        QString &error);

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

    /** POST /v1/spectral/probe — spectrum at a single pixel, computed on the
     *  node. Lets the profiler work on multi-GB remote cubes without pulling
     *  the whole cube to the client. */
    BackendSpectrumResult spectralProbe(const QString &datasetId,
                                         double x, double y) const;

    /** POST /v1/spectral/aperture — mean spectrum over a circular aperture
     *  (radius in pixels), computed on the node. */
    BackendSpectrumResult spectralAperture(const QString &datasetId,
                                            double x, double y,
                                            double radiusPixels = 3.0) const;

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

    // ── VLKB multi-cutout ────────────────────────────────────────────────────
    /** POST /v1/vlkb/mcutout/submit — one asynchronous job for a batch. */
    BackendMcutoutSubmitResult submitVlkbMcutout(const QVector<BackendMcutoutItem> &items,
                                                 const QString &serviceUrl,
                                                 const QString &bearer) const;

    /** POST /v1/vlkb/mcutout/phase — where the job has got to (starts a HELD one). */
    BackendMcutoutPhaseResult vlkbMcutoutPhase(const QString &jobId, const QString &serviceUrl,
                                               const QString &bearer) const;

    /** POST /v1/vlkb/mcutout/report — per-dataset outcome, in submission order. */
    BackendMcutoutReportResult vlkbMcutoutReport(const QString &jobId, const QString &serviceUrl,
                                                 const QString &bearer) const;

    /** POST /v1/vlkb/mcutout/fetch — download and unpack the results. */
    BackendMcutoutFetchResult vlkbMcutoutFetch(const QString &jobId, const QString &serviceUrl,
                                               const QString &bearer) const;

    // ── Simulation snapshots (AREPO) ─────────────────────────────────────────
    /** POST /v1/simulations/arepo/inspect — HDF5 tree, box size, particle types. */
    BackendArepoInspectResult inspectArepoSnapshot(const QString &path) const;

    /** POST /v1/simulations/arepo/plan — the grid a level+window would produce. */
    BackendArepoPlan planArepoExtraction(const QString &path, int level,
                                         const QVector<double> &windowKpc,
                                         const QVector<double> &offsetKpc) const;

    /** POST /v1/simulations/arepo/extract — resample to a uniform cube per field. */
    BackendArepoExtractResult extractArepoCubes(const QString &path, int level,
                                                const QString &particle,
                                                const QStringList &fields,
                                                const QVector<double> &windowKpc,
                                                const QVector<double> &offsetKpc) const;

    // ── SED fitting ──────────────────────────────────────────────────────────
    /** POST /v1/sed/fit — greybody fit of a source's photometry. */
    BackendSedFitResult requestSedFit(const BackendSedFitRequest &request) const;

    /** POST /v1/sed/models — fit against the VLKB grid of theoretical models. */
    BackendSedModelsResult requestSedModels(const QVector<BackendSedPoint> &points,
                                            double distancePc, double prefilter,
                                            double weightMidIr, double weightFarIr,
                                            double weightSubmm, double deltaChi2,
                                            const QString &serviceUrl,
                                            const QString &sourceLabel) const;

    /** GET /v1/sed/defaults — the fit ranges the backend suggests. */
    BackendSedDefaults requestSedDefaults() const;

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

    // ── R-01: CARTA script export — RETIRED (feature removed). ────────────────

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
    /// performPost, retrying while the backend answers 429 (5 attempts, 4 sleeps
    /// of 0.5 / 1 / 2 / 4 s — about 7.5 s of backoff on top of the server's own
    /// bounded wait for a compute slot).
    ///
    /// A 429 from this backend is admission control — "all this session's
    /// compute slots are busy" — not a failure: the server itself already waits
    /// for a slot, and the tasks holding them finish. Treating it as fatal
    /// turned one click on Isosurface over a large cube into "too many
    /// requests". Callers on a worker thread use this for the heavy compute
    /// endpoints; the wait is a plain sleep, so it must NOT be called on the GUI
    /// thread. On the last attempt the raw JSON body is replaced with a sentence
    /// that says the backend is busy.
    QByteArray performPostRetryingBackpressure(const QUrl &url, const QJsonDocument &body,
                                               QString &error,
                                               QMap<QString, QString> *responseHeaders = nullptr,
                                               int attempts = 5,
                                               int firstDelayMs = 500) const;

    QByteArray performPost(const QUrl &url, const QJsonDocument &body, QString &error,
                           QMap<QString, QString> *responseHeaders = nullptr,
                           int *statusOut = nullptr,
                           /// False on a 429 retry: the "started" line belongs to the
                           /// attempt the user triggered, not to each retry of it.
                           bool announceStart = true) const;
    std::chrono::milliseconds requestTimeoutFor(const QUrl &url) const;

    static QByteArray decodePayload(const QJsonObject &object, const QString &base64Field,
                                    const QString &compressionField, QString &error);

    QString m_baseUrl;
    QString m_token;
    QString m_sessionId;

    static QString s_defaultSessionId;
    /// Minted sessions are cached per (base URL + token): a session belongs to one
    /// identity, so a token rotation must mint again.
    static QHash<QString, QString> s_minted;      ///< key → session id
    /// Identities (mintKey) that answered but gave us no session — 401, 429, 5xx.
    /// Once a backend has ANSWERED we pin THAT identity to the locally generated
    /// default id for the rest of the run: silently switching session ids later
    /// would split the user's datasets across two backend sessions. Scoped per
    /// identity so one token's 401/429 does not disable minting for another.
    /// A backend that never answered is NOT pinned — nothing was stored there.
    static QSet<QString> s_mintPinnedToDefault;
    /// Base URLs whose /v1/sessions/new does not exist (404/405) — an older
    /// backend. That verdict is a property of the server, so it IS base-wide.
    static QSet<QString> s_mintUnsupportedBase;
    /// Old-token → effective-token aliases per base URL, recorded when the mint's
    /// POST had to fall back to a refreshed token. Without it a caller still
    /// holding the old token misses the cache and mints a spare session on every
    /// request. Key is mintKey(baseUrl, oldToken); value is the effective token.
    static QHash<QString, QString> s_tokenAlias;
    /// Follows s_tokenAlias to the identity a lookup should really use.
    /// Caller must hold s_sessionMutex.
    static QString resolvedToken(const QString &baseUrl, const QString &token);
    static QMutex s_sessionMutex;                 ///< guards the maps above
    /// Serialises minting so concurrent callers make ONE request and then find the
    /// cached id. Held only across a single HTTP call, which is already bounded by
    /// the client's request timeout — hence no claim/reclaim bookkeeping.
    static QMutex s_mintMutex;
    static QString mintKey(const QString &baseUrl, const QString &token);
};

#endif
