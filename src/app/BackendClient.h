#ifndef BackendClient_h
#define BackendClient_h

#include <QByteArray>
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
    QByteArray performPost(const QUrl &url, const QJsonDocument &body, QString &error) const;
    std::chrono::milliseconds requestTimeoutFor(const QUrl &url) const;

    static QByteArray decodePayload(const QJsonObject &object, const QString &base64Field,
                                    const QString &compressionField, QString &error);

    QString m_baseUrl;
    QString m_token;
    QString m_sessionId;
};

#endif
