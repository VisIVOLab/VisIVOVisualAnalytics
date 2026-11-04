#ifndef VOLUMERAYTRACEPANEL_H
#define VOLUMERAYTRACEPANEL_H

// The settings of a ray-traced cube (issue #106): its style (3-D light, or a
// projected map), its light (from the cube's values, their contrast), its
// colour (from the same values or from another field of a VBT volume: a
// two-field colour table) and its tone. In the cube viewer's sidebar, under
// "Ray-traced light"; the stills and movies render what it sets.

#include "ParticleGLView.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QToolButton;

class VolumeRaytracePanel : public QWidget
{
    Q_OBJECT

public:
    explicit VolumeRaytracePanel(QWidget *parent = nullptr);

    /// The other fields the colour can come from (a VBT volume's): their
    /// names, by field index; the cube's own field is left out.
    void setFields(const QList<QPair<QString, int>> &fields);
    /// Starts the map from the 3-D view's, when it is one of these.
    void setMap(const QString &name);
    /// Whether the 3-D view's threshold darkens what lies below it: a radio
    /// cube's noise, which a ray sums over every channel. The default: on for
    /// a cube with negative values (noise), off for a simulation's density.
    bool thresholdDarkens() const;
    void setThresholdDarkens(bool on);

    bool projected() const;
    /// The request fields these settings set (app/routers/volume_render.py,
    /// VolumeRenderRequest) without the dataset, box, cameras, size, tone.
    QJsonObject fields() const;
    /// The tone fields over an exposure ``base`` (0: the backend's automatic).
    QJsonObject tone(double base) const;
    /// The same for the GPU view, with the grid's colour table and the range
    /// of its quantity (the colour bar's).
    ParticleGLView::Tone gpuTone(const std::vector<QVector3D> &lut, double lo, double hi,
                                 bool log, const QString &brightnessName) const;
    /// What in a request the backend prepares the grid from: a new key, a new
    /// grid to fetch.
    static QString gridKey(const QJsonObject &request);

signals:
    /// The grid or the style changed: it must be fetched again.
    void gridChanged();
    /// Only the tone (or the absorption, the quality) changed: drawn again.
    void toneChanged();

private:
    void syncRows();
    static void setRowVisible(QWidget *field, bool visible);

    QComboBox *m_mode{ nullptr };
    QCheckBox *m_lightLog{ nullptr };
    QDoubleSpinBox *m_contrast{ nullptr };
    QComboBox *m_colour{ nullptr };
    QComboBox *m_map{ nullptr };
    QCheckBox *m_mapLog{ nullptr };
    QDoubleSpinBox *m_exposureEv{ nullptr };
    QDoubleSpinBox *m_absorption{ nullptr };
    QDoubleSpinBox *m_quality{ nullptr };
    QDoubleSpinBox *m_slab{ nullptr };
    QDoubleSpinBox *m_decades{ nullptr };
    QComboBox *m_curve{ nullptr };
    QDoubleSpinBox *m_stretch{ nullptr };
    QDoubleSpinBox *m_gamma{ nullptr };
    QDoubleSpinBox *m_saturation{ nullptr };
    QCheckBox *m_floor{ nullptr };
    QToolButton *m_more{ nullptr };
    QWidget *m_advanced{ nullptr };   ///< the rows under "More settings"
};

#endif // VOLUMERAYTRACEPANEL_H
