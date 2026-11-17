#ifndef PvDiagramWidget_h
#define PvDiagramWidget_h

#include <QVector>
#include <QWidget>

#include "LutScale.h"

#include <vector>

class QCheckBox;
class QLabel;
class QSpinBox;
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

    /// The colour stretch, as on a map pane: Linear, Log or Sqrt (Square and
    /// Power through the same curve). Kept across new data, so a re-taken cut
    /// comes back on it. False, and nothing changes, for Log on a diagram with
    /// no positive value.
    bool setScale(LutScale::Mode mode);
    LutScale::Mode scale() const { return m_scale; }

signals:
    void widthChanged(int widthPixels);
    void smoothChanged(bool smooth);
    void newPathRequested();
    void saveFitsRequested();
    /// Log was asked for, or held over a new cut, on a diagram with no positive
    /// value; the stretch is (or stays) linear.
    void logScaleUnavailable();

private slots:
    void savePng();

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
    QCheckBox      *m_smoothCheck{ nullptr };
};

#endif
