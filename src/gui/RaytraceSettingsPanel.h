#ifndef RAYTRACESETTINGSPANEL_H
#define RAYTRACESETTINGSPANEL_H

// The settings of a ray-traced particle render (issue #101): its style (a 3-D
// render, or a projected map), its particles (size, light, contrast) and its
// tone. Shared by the point viewer's "Ray-traced" shape (in its sidebar) and
// the render dialog, so a still renders what the view shows.

#include "ParticleGLView.h"

#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QHBoxLayout;
class QSpinBox;
class QToolButton;

class RaytraceSettingsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RaytraceSettingsPanel(const QStringList &numericColumns, QWidget *parent = nullptr);

    bool projected() const;
    double absorption() const;
    /// The request fields these settings set (app/routers/particle_render.py):
    /// sizes, light, style; merged over the view's positions, filters, colours.
    QJsonObject fields() const;
    /// The tone fields, the exposure ``base`` (0: the backend's automatic
    /// one) and the stops chosen over it.
    QJsonObject tone(double base) const;
    /// The same for the GPU view; ``viewColour``: the view's colour request
    /// (its colour function, for a map's hue by the view's colours).
    ParticleGLView::Tone gpuTone(const QJsonObject &viewColour) const;
    /// What the backend prepares the particles from, in a request: a new key,
    /// new particles to fetch (a projected map's colours are drawn here).
    static QString particlesKey(const QJsonObject &request);

    /// All of it, to start another panel from this one.
    QJsonObject settings() const;
    void setSettings(const QJsonObject &settings);

signals:
    /// The particles or the style changed: what is drawn must be fetched again.
    void particlesChanged();
    /// Only the tone (or a map's colours) changed: what is drawn is graded again.
    void toneChanged();

private:
    void syncRows();
    static void setRowVisible(QWidget *field, bool visible);

    QComboBox *m_mode{ nullptr };
    QGroupBox *m_projectedBox{ nullptr };
    QComboBox *m_colourBy{ nullptr };
    QComboBox *m_map{ nullptr };
    QCheckBox *m_mapLog{ nullptr };
    QDoubleSpinBox *m_slab{ nullptr };
    QDoubleSpinBox *m_decades{ nullptr };
    QComboBox *m_size{ nullptr };
    QSpinBox *m_neighbours{ nullptr };
    QDoubleSpinBox *m_sizeScale{ nullptr };
    QComboBox *m_light{ nullptr };
    QCheckBox *m_lightLog{ nullptr };
    QDoubleSpinBox *m_contrast{ nullptr };
    QComboBox *m_quantity{ nullptr };
    QDoubleSpinBox *m_absorption{ nullptr };
    QDoubleSpinBox *m_exposureEv{ nullptr };
    QComboBox *m_curve{ nullptr };
    QDoubleSpinBox *m_stretch{ nullptr };
    QDoubleSpinBox *m_gamma{ nullptr };
    QDoubleSpinBox *m_saturation{ nullptr };
    // The essentials show; the rest behind "More settings".
    QToolButton *m_more{ nullptr };
    QFormLayout *m_particlesForm{ nullptr };
    QHBoxLayout *m_sizeRow{ nullptr };
};

#endif // RAYTRACESETTINGSPANEL_H
