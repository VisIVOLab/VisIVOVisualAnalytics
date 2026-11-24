#ifndef PvDiagramWidget_h
#define PvDiagramWidget_h

#include <QVector>
#include <QWidget>

#include "LutScale.h"

#include <limits>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QCPAbstractItem;
class QCustomPlot;
class QCPColorMap;
class QCPColorScale;
class QCPTextElement;

class PvDiagramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PvDiagramWidget(QWidget *parent = nullptr);

    /// Structured path metadata shown in the header panel.
    /// vtkWindowCube fills this from the RemotePvFetchResult; the widget
    /// only renders it — no business logic inside.
    struct PvPathInfo {
        QString startPoint;
        QString endPoint;
        int     vertexCount              { 0 };
        int     widthPixels              { 1 };
        double  lengthPixels             { 0.0 };
        QString spectralAxisLabel;
        QString spatialUnit;             ///< e.g. "arcsec" or "pixels"
        double  pixelScaleArcsecPerPixel { 0.0 };
        QString bunit;                   ///< colour-bar label
        /// What one pixel of width is on the sky (arcsec); 0 when unknown. The
        /// width is shown, and can be typed, in arcsec next to pixels.
        double  stepArcsec               { 0.0 };
        /// Each vertex on the diagram's offset axis, in the axis' own unit, for
        /// the marks numbered as on the image; empty when the path is a curve
        /// (its vertices are not the cube's).
        QVector<double> vertexOffsets;
        QString vertexOffsetsText;       ///< "1 at 0.0 arcsec, 2 at ..." for the header
        QString channelsText;            ///< the channels the diagram covers; empty: whole band
        QString spectralUnit;            ///< unit of the range's values (km/s, Hz); empty: unknown
    };

    /// The spectral range typed in the panel (issue #520). @p byValue: the
    /// numbers are values of the spectral axis, else 1-based channels. NaN: no
    /// bound on that side.
    struct SpectralRangeInput {
        bool   byValue { false };
        double from    { std::numeric_limits<double>::quiet_NaN() };
        double to      { std::numeric_limits<double>::quiet_NaN() };
    };

    /// @p spectral: the coordinate of each channel, channel 0 first — it may
    /// DEcrease (CDELT3 < 0); rows are placed by value, never by index.
    void setPvData(const QVector<double> &positions,
                   const QVector<double> &spectral,
                   const QVector<double> &intensities,
                   int nx, int ny,
                   const QString &xLabel,
                   const QString &yLabel,
                   const QString &title,
                   const PvPathInfo &info);

    /// Apply dark (true) or light (false) plot theme.
    void applyPlotTheme(bool dark);

    /// Reflect the path settings in the controls without emitting the signals.
    void setPathControls(int widthPixels, bool smooth);
    /// ... and the spectral range, with the unit of the axis' values.
    void setRangeControls(const SpectralRangeInput &range, const QString &valueUnit);

    /// The colour stretch, as on a map pane: Linear, Log or Sqrt (Square and
    /// Power through the same curve). Kept across new data, so a re-taken cut
    /// comes back on it. False, and nothing changes, for Log on a diagram with
    /// no positive value.
    bool setScale(LutScale::Mode mode);
    LutScale::Mode scale() const { return m_scale; }

signals:
    void widthChanged(int widthPixels);
    /// A From/To bound or the unit was edited.
    void spectralRangeChanged(bool byValue, double from, double to);
    /// Write the path (and optionally the polygon of its width) as regions.
    void exportPathAsRegionRequested();
    /// Take the selected line region as the path.
    void pathFromRegionRequested();
    void smoothChanged(bool smooth);
    void newPathRequested();
    void saveFitsRequested();
    /// Log was asked for, or held over a new cut, on a diagram with no positive
    /// value; the stretch is (or stays) linear.
    void logScaleUnavailable();

private slots:
    void savePng();
    void rangeEdited();
    void widthArcsecEdited();

private:
    void buildHeaderUi();
    void updateHeaderUi(const PvPathInfo &info);
    /// Put m_scale on the colour map and its bar: data range, gradient, ticker.
    void applyScale();

    LutScale::Mode m_scale{ LutScale::Mode::Linear };
    double m_dataMin{ 0.0 };
    double m_dataMax{ 1.0 };
    double m_logFloor{ 0.0 };   ///< 1st percentile of the positive values; 0 = none
    std::vector<double> m_cells;   ///< the diagram's values, row-major, as received
    int m_cellColumns{ 0 };
    int m_cellRows{ 0 };

    // ── Header labels (Path group) ────────────────────────────────────────────
    QLabel *m_lblStart    { nullptr };
    QLabel *m_lblEnd      { nullptr };
    QLabel *m_lblVertices { nullptr };
    QLabel *m_lblLength   { nullptr };
    // ── Header labels (Sampling / Axes group) ─────────────────────────────────
    QLabel *m_lblWidth    { nullptr };
    QLabel *m_lblSpectral { nullptr };
    QLabel *m_lblSpatial  { nullptr };
    QLabel *m_lblScale    { nullptr };

    // ── Plot ──────────────────────────────────────────────────────────────────
    QCustomPlot    *plot         { nullptr };
    QCPColorMap    *colorMap     { nullptr };
    QCPColorScale  *colorScale   { nullptr };
    QCPTextElement *titleElement { nullptr };
    QSpinBox       *m_widthSpin  { nullptr };
    QDoubleSpinBox *m_widthArcsecSpin { nullptr };
    double          m_stepArcsec { 0.0 };
    SpectralRangeInput m_lastRange;   ///< what the range controls last said
    QCheckBox      *m_smoothCheck{ nullptr };
    QComboBox      *m_rangeMode  { nullptr };
    QLineEdit      *m_rangeFrom  { nullptr };
    QLineEdit      *m_rangeTo    { nullptr };
    QLabel         *m_lblChannels { nullptr };
    QVector<QCPAbstractItem *> m_vertexItems;   ///< the numbered marks on the offset axis
};

#endif
