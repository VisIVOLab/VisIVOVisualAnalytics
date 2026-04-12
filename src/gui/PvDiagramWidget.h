#ifndef PvDiagramWidget_h
#define PvDiagramWidget_h

#include <QVector>
#include <QWidget>

class QLabel;
class QCustomPlot;
class QCPColorMap;
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
    };

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

private slots:
    void savePng();

private:
    void buildHeaderUi();
    void updateHeaderUi(const PvPathInfo &info);

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
    QCPTextElement *titleElement { nullptr };
};

#endif
