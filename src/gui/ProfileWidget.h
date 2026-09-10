#ifndef ProfileWidget_h
#define ProfileWidget_h

#include "AstroUtils.h"

#include <vtkSmartPointer.h>

#include <QColor>
#include <QWidget>
#include <QVector>

#include <memory>

class QCPGraph;
class QCPItemLine;
class QCPItemText;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QToolButton;
class vtkImageData;
class vtkInteractorStyleProfile;
struct BackendLineFitResult;

QT_BEGIN_NAMESPACE
namespace Ui {
class ProfileWidget;
}
QT_END_NAMESPACE

class ProfileWidget : public QWidget
{
    Q_OBJECT

public:
    enum class UsageMode
    {
        ProbeLive,
        RegionStatic,
    };

    explicit ProfileWidget(vtkInteractorStyleProfile *style, vtkImageData *dataset,
                           const std::string &filepath, QWidget *parent = nullptr);
    explicit ProfileWidget(QWidget *parent = nullptr);
    ~ProfileWidget();

    void setUsageMode(UsageMode mode, const QString &windowTitle = QString());
    void setupImagePlots();
    void setupImagePlots(const QString &xLabel, const QString &yLabel);
    void setupSpectrumPlot();
    void setupSpectrumPlot(const QString &xLabel, const QString &yLabel);
    void updateSpectrumPlot(const QVector<double> &key, const QVector<double> &values,
                            const QString &title, bool live);
    void updateSpectrumPlotSeries(const QVector<double> &key,
                                  const QVector<double> &primaryValues,
                                  const QString &primaryLabel,
                                  const QVector<double> &secondaryValues,
                                  const QString &secondaryLabel,
                                  const QString &title, bool live);
    void updateImageProfiles(const QVector<double> &keyX, const QVector<double> &valuesX,
                             const QVector<double> &keyY, const QVector<double> &valuesY,
                             double probeX, double probeY, bool live);

    /// Apply dark (true) or light (false) plot theme to all active plots.
    void applyPlotTheme(bool dark);

    /// Populate the spectrum header meta row. Pass empty strings to omit a
    /// field. Pixel coordinates are shown verbatim; the optional wcsLabel is
    /// the formatted RA/Dec or Galactic l/b string the caller already
    /// computed (so we don't have to depend on AstroUtils here).
    void setSpectrumContext(const QString &fileLabel,
                            int pixelX, int pixelY,
                            const QString &wcsLabel = QString());

    /// Show / hide the "🔒 Frozen at …" badge in the header. When false, the
    /// header shows the live indicator instead. The cube viewer sets this
    /// from its own probeFrozen state; the widget does NOT auto-toggle.
    void setSpectrumFrozen(bool frozen);

    /// Region-mode header replacement. Call after setUsageMode(RegionStatic):
    /// hides the Live/Pinned badge and the pixel/hover hint, and shows a
    /// region description instead (e.g. "Circle · 195 / 195 valid pixels").
    /// The optional @p extraInfo line is appended below (e.g. "computed over
    /// the current 2D slice / all cube channels").
    void setRegionContext(const QString &shapeLabel,
                          int validPixels, int totalPixels,
                          const QString &extraInfo = QString());

    /// Generic (non-region) header line for a 1-D product mounted in a pane (Stack, Pin, …).
    /// Replaces the region-pixel semantics of setRegionContext so a reused slot does not keep
    /// showing "Region spectrum" / the previous product's description.
    void setSpectrumMeta(const QString &metaLine, const QString &extraLine = QString());

    /// Refresh the spectrum axis titles without re-running setupSpectrumPlot (which would
    /// accumulate title rows). Needed when a slot is reused for a product with different axes.
    void setSpectrumAxisLabels(const QString &xLabel, const QString &yLabel);

    /// Mark a channel as the "current" one (the slice the cube viewer is
    /// displaying). Drawn as a vertical reference line on the spectrum.
    /// Pass spectralValueAtChannel = NaN to clear the marker.
    void setSpectrumCurrentChannel(double spectralValueAtChannel);

    /// Give the widget the backend coordinates so the "Fit Gaussian" button can
    /// call POST /v1/spectral/linefit on the current spectrum. Without this the
    /// fit button stays disabled.
    void setBackend(const QString &url, const QString &token);

    /// Pin the currently displayed spectrum as a persistent comparison curve
    /// (spectrum mode only). Subsequent live / region updates keep the pinned
    /// curves so several regions' spectra can be overlaid and compared. Each
    /// pin gets a distinct colour and a legend entry. No-op if there is no
    /// current spectrum. @p label names the legend entry (falls back to the
    /// current plot title, then an auto index).
    void pinCurrentSpectrum(const QString &label = QString());
    /// Drop all pinned comparison curves and hide the legend.
    void clearPinnedSpectra();
    int  pinnedSpectrumCount() const { return static_cast<int>(m_pinnedGraphs.size()); }

    // ── Pane migration (the spectrum controls move to Inspector › Analysis) ────
    // When this ProfileWidget is EMBEDDED as a pane product, its own header toolbar /
    // stats bar / save footer are hidden and the identical controls are driven from the
    // Inspector via the public methods below. A standalone (probe/region window) widget
    // keeps its inline chrome. See vtkWindowCube::updateSpectrumInspectorControls().

    /// Hide (true) or show (false) the inline chrome — header toolRow, stats bar, save
    /// footer — leaving just the plot. No-op before setupSpectrumPlot() built them.
    void setPaneChromeHidden(bool hidden);

    /// Public action wrappers so the Inspector's Spectrum panel can drive THIS widget.
    void applySmoothingCode(int code);           ///< set the smoothing kernel (combo data value)
    int  smoothingCode() const;                  ///< current kernel code (0 = none)
    void fitGaussianNow();                        ///< run the backend Gaussian fit + overlay
    void clearFitNow();                           ///< remove the fitted-curve overlay + its label
    /// Spectral-axis context for the line overlay: the axis CTYPE (FREQ / VOPT /
    /// VRAD / VELO), the unit its values are in, and the cube's own rest
    /// frequency in Hz (0 when the header has no RESTFRQ). Without these a rest
    /// -frequency line list cannot be placed on the axis at all.
    void setSpectralAxisContext(const QString &ctype, const QString &unit, double restFreqHz);
    /// Dataset key for parameter persistence (AnalysisParamStore), so the line
    /// list's unit and systemic velocity are remembered per cube — every other
    /// parameter dialog in the app does this, this one made you retype them.
    void setDatasetPersistKey(const QString &key) { m_datasetPersistKey = key; }
    bool hasFit() const { return m_fitGraph != nullptr; }   ///< a fit is currently overlaid
    void loadLinesFromFile();                     ///< open the Line-ID CSV picker + overlay
    void clearLines();                            ///< remove the Line-ID overlay
    void saveSpectrumCsvNow();                     ///< export the raw spectrum as CSV
    void saveSpectrumPngNow();                     ///< export the plot as PNG
    bool canFit() const { return !m_backendUrl.isEmpty() && !m_fitInProgress; }   ///< can START a fit
    bool hasSpectrum() const { return !m_lastSpectrumValues.isEmpty(); }
    QString statsText() const;                    ///< the current stats-bar text (or empty)

signals:
    /// Emitted whenever the displayed spectrum / its stats / pin set changes, so an external
    /// (Inspector) controls panel can refresh its stats readout + enabled/pin state.
    void spectrumChanged();

    /// Emitted when the user toggles the Pin button in the header. The cube
    /// viewer connects this to its probeFrozen state so the spectrum freezes
    /// in place even when the mouse keeps moving over the slice.
    void freezeToggled(bool frozen);

private slots:
    void savePlot1Png();
    void savePlot2Png();
    /// Save the current spectrum (key + values) as a CSV: header line +
    /// two columns. Only meaningful in spectrum-mode — the button is
    /// hidden in image-profile mode.
    void saveSpectrumCsv();
    /// Fit a Gaussian to the current spectrum via the backend and overlay it.
    void requestLineFit();

private:
    Ui::ProfileWidget *ui;
    vtkSmartPointer<vtkImageData> dataset;
    std::unique_ptr<AstroUtils> astro;

    float *ScalarPointer;
    int Dimensions[3];
    vtkIdType Increments[3];

    vtkSmartPointer<vtkInteractorStyleProfile> interactor;
    QCPItemLine *refLineX;
    QCPItemLine *refLineY;
    QCPItemLine *m_channelMarker{ nullptr };
    UsageMode usageMode{ UsageMode::ProbeLive };

    QPushButton *m_btnSavePlot1 { nullptr };
    QPushButton *m_btnSavePlot2 { nullptr };
    QPushButton *m_btnSaveCsv   { nullptr };
    bool m_darkTheme{ true };

    // ── Multi-region comparison (pinned spectra) ────────────────────────────
    // Persistent overlay curves the user "pins" from successive region/probe
    // spectra so they can be compared. graph(0) is always the live/current
    // series; pinned graphs are appended after it and survive live updates.
    QVector<QCPGraph *> m_pinnedGraphs;
    QString m_lastSpectrumTitle;   ///< last title passed to updateSpectrumPlot (pin label default)
    QPushButton *m_btnPin       { nullptr };
    QPushButton *m_btnClearPins { nullptr };
    void refreshPinButtons();
    void applyPinnedLegendTheme();
    static QColor pinColour(int index);
    // Tracks whether we've already shown the window once. raise() and
    // activateWindow() must only run on the very first appearance — if we
    // call them on every hover update (which happens many times per second
    // during live probe), they steal focus from any modal native dialog
    // currently attached to this window (e.g. the Save-as file picker),
    // which on macOS shows up as the file picker closing by itself.
    bool m_initiallyRaised{ false };
    // True while a Save dialog spawned by this widget is open. Suppresses
    // all internal redraw / state-mutation paths (plotSpectrum, refreshes,
    // channel marker) for the dialog's lifetime — even ApplicationModal
    // doesn't stop timers / async callbacks from firing, and on macOS any
    // mutation of the parent window can dismiss an NSSavePanel sheet.
    bool m_dialogInProgress{ false };
    void showAndRaiseFirstTime();

    // Cached current spectrum (last data passed to updateSpectrumPlot /
    // plotSpectrum), used for CSV export. Two parallel vectors of the same
    // length: m_lastSpectrumKey holds the x-axis values (spectral coord)
    // and m_lastSpectrumValues the y-axis intensity. Empty until first draw.
    QVector<double> m_lastSpectrumKey;
    QVector<double> m_lastSpectrumValues;
    QString         m_lastSpectrumXLabel;
    QString         m_lastSpectrumYLabel;

    // ── Gaussian line fit (backend /v1/spectral/linefit) ────────────────────
    QString      m_backendUrl;
    QString      m_backendToken;
    QPushButton *m_btnFit   { nullptr };
    QCPGraph    *m_fitGraph { nullptr };   ///< overlaid fitted-curve graph
    QCPItemText *m_fitLabel { nullptr };   ///< on-plot fit parameter annotation
    bool         m_fitInProgress { false };
    // Bumped every time the displayed spectrum changes; a fit reply whose
    // captured generation no longer matches is stale and is discarded (the
    // live probe may have moved on while the backend was computing).
    int          m_spectrumGeneration { 0 };
    void overlayLineFit(const BackendLineFitResult &fit);
    void clearFitOverlay();

    // ── Spectrum-mode header chrome ─────────────────────────────────────────
    // Created lazily when setupSpectrumPlot() runs so image-mode users don't
    // pay for the extra widgets.
    QFrame      *m_specHeader     { nullptr };
    QLabel      *m_specTitleLabel { nullptr };
    QLabel      *m_specMetaLabel  { nullptr };
    QLabel      *m_specStatusLabel{ nullptr };
    QToolButton *m_specPinBtn     { nullptr };
    QFrame      *m_specStatsBar   { nullptr };
    QLabel      *m_specStatsLabel { nullptr };
    QFrame      *m_specFooter     { nullptr };   ///< save-buttons footer (hidden in pane mode)

    QString m_fileLabel;
    int     m_pixelX{ 0 };
    int     m_pixelY{ 0 };
    QString m_wcsLabel;
    bool    m_frozen{ false };

    // Spectral smoothing — applied client-side for display only (the raw
    // data is kept in m_lastSpectrum* for CSV export). The combo is built
    // lazily in buildSpectrumHeader().
    QComboBox *m_smoothCombo{ nullptr };
    void applySmoothAndReplot();
    /// The spectrum values as currently DISPLAYED — raw m_lastSpectrumValues
    /// with the active smoothing kernel applied (identity when smoothing is
    /// off). Used by both applySmoothAndReplot() and pinCurrentSpectrum() so a
    /// pinned curve matches exactly what was on screen when it was pinned.
    QVector<double> displayedSpectrumValues() const;

    // Line identification overlay — vertical annotated markers at known spectral
    // lines read from a file. What the file carries is REST frequencies; what is
    // stored here is the position on THIS axis, after the redshift shift and the
    // unit / velocity-convention conversion (see loadLineIdFile).
    struct SpectralLine {
        double frequency;   // in the same unit as the X axis
        QString label;
    };
    QVector<SpectralLine> m_lineIds;
    QString m_axisCtype;         ///< spectral CTYPE of the plotted axis
    QString m_axisUnit;          ///< unit of the plotted axis values
    double  m_axisRestFreqHz{ 0.0 };   ///< cube RESTFRQ in Hz (0 = unknown)
    QString m_datasetPersistKey;       ///< for AnalysisParamStore (may be empty)
    QVector<QCPItemLine*> m_lineIdMarkers;
    QVector<QCPItemText*> m_lineIdLabels;
    void loadLineIdFile();
    void drawLineIdOverlay();
    /// A line read from the file, before any conversion.
    struct RestLine {
        double value{ 0.0 };   ///< in the file's own unit
        QString label;
    };
    /// Parse a line list: a 2-column `value,label` CSV, or a catalogue export
    /// (Splatalogue / CDMS style) whose header row names the columns. Returns the
    /// rows plus, when the file says so, a unit hint ("GHz").
    /// @p allowHeader false forces the two-column reading. @p retryAsTwoColumn is
    /// set when the result is empty ONLY because the file's single row was read
    /// as a header — the caller then re-reads with @p allowHeader false. A real
    /// catalogue header followed by unusable data rows does NOT set it: those
    /// rows must stay rejected, not be reinterpreted (a row index read as a
    /// frequency invents a line at 1 GHz).
    static QVector<RestLine> parseLineListFile(const QString &text, QString &unitHint,
                                               QString &formatNote, bool allowHeader = true,
                                               bool *retryAsTwoColumn = nullptr);
    /// Shift the rest-frame @p lines to the observed frame and convert them to
    /// this plot's axis, then draw them. @p mode: 0 = systemic velocity in km/s
    /// (optical), 1 = redshift z, 2 = the values are already observed.
    void applyLineList(const QVector<RestLine> &lines, const QString &unitText, int mode,
                       double shift);
    void clearLineIdOverlay();
    // Region-mode header text. Populated by setRegionContext(); used by
    // refreshSpectrumHeader() when usageMode == RegionStatic so the meta
    // row describes the region instead of a pixel.
    QString m_regionMetaLine;
    QString m_regionExtraLine;

    void plotProfile(double x, double y, bool live);
    void plotSpectrum(double x, double y, bool live);
    void applyUsageModeUi(const QString &windowTitle = QString());
    void buildSpectrumHeader();
    void refreshSpectrumHeader();
    void refreshSpectrumStats(const QVector<double> &spectralKey,
                              const QVector<double> &values);
};

#endif
