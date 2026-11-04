#include "RaytraceSettingsPanel.h"

#include "ColormapIcon.h"
#include "vtk/ColorMaps.h"

#include <QCheckBox>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <vtkLookupTable.h>
#include <vtkNew.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>

using namespace Qt::Literals::StringLiterals;

namespace {

/// 256 colours of a named map, as the backend's map of that name.
std::vector<QVector3D> namedLut(const QString &name)
{
    vtkNew<vtkLookupTable> lut;
    ColorMaps::SetColorMap(lut, name.toStdString());
    const vtkIdType n = std::max<vtkIdType>(1, lut->GetNumberOfTableValues());
    std::vector<QVector3D> out;
    for (int i = 0; i < 256; ++i) {
        double rgba[4];
        lut->GetTableValue(static_cast<vtkIdType>(std::lround(i * (n - 1) / 255.0)), rgba);
        out.emplace_back(float(rgba[0]), float(rgba[1]), float(rgba[2]));
    }
    return out;
}

/// 256 colours of the view's own colour function ([[x, r, g, b], …]) over
/// [lo, hi], evenly in the logarithm when ``log``.
std::vector<QVector3D> pointsLut(const QJsonArray &points, double lo, double hi, bool log)
{
    std::vector<std::array<double, 4>> nodes;
    for (const QJsonValue &v : points) {
        const QJsonArray p = v.toArray();
        nodes.push_back({ p.at(0).toDouble(), p.at(1).toDouble(), p.at(2).toDouble(),
                          p.at(3).toDouble() });
    }
    std::sort(nodes.begin(), nodes.end());
    std::vector<QVector3D> out;
    for (int i = 0; i < 256; ++i) {
        const double f = i / 255.0;
        const double x = log && lo > 0 ? lo * std::pow(hi / lo, f) : lo + (hi - lo) * f;
        std::array<double, 4> c{ x, f, f, f };
        if (!nodes.empty()) {
            if (x <= nodes.front()[0]) {
                c = nodes.front();
            } else if (x >= nodes.back()[0]) {
                c = nodes.back();
            } else {
                const auto it = std::upper_bound(
                        nodes.begin(), nodes.end(), c,
                        [](const auto &a, const auto &b) { return a[0] < b[0]; });
                const auto &b = *it, &a = *(it - 1);
                // Between nodes, in the logarithm on a log scale, as the
                // backend's colour function interpolates them.
                const double t = log && a[0] > 0 && x > 0
                        ? std::log(x / a[0]) / std::max(1e-300, std::log(b[0] / a[0]))
                        : (x - a[0]) / std::max(1e-300, b[0] - a[0]);
                for (int k = 1; k < 4; ++k)
                    c[k] = a[k] + (b[k] - a[k]) * t;
            }
        }
        out.emplace_back(float(c[1]), float(c[2]), float(c[3]));
    }
    return out;
}

QDoubleSpinBox *spin(double lo, double hi, double step, double value, int decimals = 2)
{
    auto *box = new QDoubleSpinBox;
    box->setRange(lo, hi);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    box->setValue(value);
    return box;
}

} // namespace

RaytraceSettingsPanel::RaytraceSettingsPanel(const QStringList &numericColumns, QWidget *parent)
    : QWidget(parent)
{
    // Style.
    auto *style = new QGroupBox(u"Style"_s);
    auto *sf = new QFormLayout(style);
    m_mode = new QComboBox;
    m_mode->addItem(u"3-D light"_s, u"light"_s);
    m_mode->addItem(u"Projected map"_s, u"projected"_s);
    m_mode->setToolTip(u"3-D light: the particles emit and absorb light, in perspective.\n"
                       u"Projected map: as cosmological simulations are shown, without "
                       u"perspective, the brightness the logarithm of the projected density, "
                       u"the colour the density or another quantity."_s);
    sf->addRow(u"Mode"_s, m_mode);

    m_projectedBox = new QGroupBox(u"Projected map"_s);
    auto *mapf = new QFormLayout(m_projectedBox);
    m_colourBy = new QComboBox;
    m_colourBy->addItem(u"Density"_s, u"density"_s);
    m_colourBy->addItem(u"View's colours"_s, u"colour"_s);
    m_colourBy->addItem(u"Velocity dispersion"_s, u"velocity_dispersion"_s);
    m_colourBy->setToolTip(u"The hue: the density itself, the view's colour column averaged "
                           u"along each ray, or the local velocity dispersion (from the "
                           u"VX, VY, VZ columns)"_s);
    mapf->addRow(u"Colour by"_s, m_colourBy);
    m_map = new QComboBox;
    m_map->setIconSize(QSize(28, 10));   // each with its gradient, as the image viewer's
    for (const QString &name : { u"Magma"_s, u"Inferno"_s, u"Plasma"_s, u"Viridis"_s,
                                 u"Cividis"_s, u"Glow"_s, u"Temperature"_s, u"Coolwarm"_s,
                                 u"Gray"_s })
        m_map->addItem(makeColormapIcon(name.toStdString()), name, name);
    m_mapLog = new QCheckBox(u"log"_s);
    m_mapLog->setChecked(true);
    m_mapLog->setToolTip(u"The quantity of the hue on a log scale"_s);
    // The map on a row of its own: its gradient and its name both show.
    mapf->addRow(u"Map"_s, m_map);
    m_mapLog->setText(u"Log scale"_s);
    mapf->addRow(QString(), m_mapLog);
    m_slab = spin(0.0, 1.0e9, 1.0, 0.0);
    m_slab->setSpecialValueText(u"Everything"_s);
    m_slab->setToolTip(u"Only what lies within this thickness of the focal plane, in the "
                       u"data's units (the Millennium images: a 15 Mpc/h slab)"_s);
    mapf->addRow(u"Slab"_s, m_slab);
    m_decades = spin(0.0, 12.0, 0.5, 0.0);
    m_decades->setSpecialValueText(u"Automatic"_s);
    m_decades->setSuffix(u" dec"_s);
    m_decades->setToolTip(u"The decades of density the brightness spans, down from the "
                          u"brightest"_s);
    mapf->addRow(u"Range"_s, m_decades);

    // Particles.
    auto *particles = new QGroupBox(u"Particles"_s);
    auto *pf = new QFormLayout(particles);
    m_particlesForm = pf;
    m_size = new QComboBox;
    m_size->addItem(u"From neighbours"_s, QString());
    m_light = new QComboBox;
    m_light->addItem(u"Same for all"_s, QString());
    for (const QString &c : numericColumns) {
        m_size->addItem(c, c);
        m_light->addItem(c, c);
    }
    m_size->setToolTip(u"Each particle is a Gaussian of this size: a smoothing length column, "
                       u"or half the distance to its N-th nearest particle."_s);
    pf->addRow(u"Size"_s, m_size);
    m_neighbours = new QSpinBox;
    m_neighbours->setRange(1, 512);
    m_neighbours->setValue(32);
    m_neighbours->setPrefix(u"N = "_s);
    m_sizeScale = spin(0.05, 100.0, 0.1, 1.0);
    m_sizeScale->setPrefix(u"× "_s);
    m_sizeRow = new QHBoxLayout;
    m_sizeRow->addWidget(m_neighbours);
    m_sizeRow->addWidget(m_sizeScale);
    pf->addRow(QString(), m_sizeRow);
    m_lightLog = new QCheckBox(u"log"_s);
    m_lightLog->setToolTip(u"The column holds the logarithm of the quantity (LogMass, LogD…)"_s);
    auto *lightRow = new QHBoxLayout;
    lightRow->addWidget(m_light, 1);
    lightRow->addWidget(m_lightLog);
    pf->addRow(u"Light"_s, lightRow);
    m_contrast = spin(0.25, 4.0, 0.1, 1.0);
    m_contrast->setToolTip(u"The light raised to this power. Above 1 the faint, diffuse "
                           u"particles fade and the dense structure stands out (2: a gas's "
                           u"density squared)."_s);
    pf->addRow(u"Contrast"_s, m_contrast);
    m_quantity = new QComboBox;
    m_quantity->addItem(u"Per particle"_s, u"per_particle"_s);
    m_quantity->addItem(u"Density"_s, u"density"_s);
    m_quantity->setToolTip(u"What the light measures. An amount per particle (mass, "
                           u"luminosity): a particle sends its light however far it is spread. "
                           u"A density: a ray sees the column it crosses, so a gas shows its "
                           u"projected density."_s);
    pf->addRow(u"Measures"_s, m_quantity);
    m_absorption = spin(0.0, 20.0, 0.25, 0.0);
    m_absorption->setToolTip(u"Grey absorption, as the optical depth where the light is "
                             u"brightest. 0: emission only; 1 or more: dense regions hide "
                             u"what lies behind them (in the still; the GPU view shows "
                             u"emission)."_s);
    pf->addRow(u"Absorption"_s, m_absorption);

    // Tone.
    auto *grade = new QGroupBox(u"Tone"_s);
    auto *gf = new QFormLayout(grade);
    m_exposureEv = spin(-12.0, 12.0, 0.5, 0.0);
    m_exposureEv->setSuffix(u" EV"_s);
    m_exposureEv->setToolTip(u"Relative to the automatic exposure: +1 doubles the light."_s);
    gf->addRow(u"Exposure"_s, m_exposureEv);
    m_curve = new QComboBox;
    m_curve->addItem(u"Asinh"_s, u"asinh"_s);
    m_curve->addItem(u"Logarithmic"_s, u"log"_s);
    m_curve->addItem(u"Exponential"_s, u"exponential"_s);
    m_curve->addItem(u"Linear"_s, u"linear"_s);
    gf->addRow(u"Curve"_s, m_curve);
    m_stretch = spin(0.1, 100000.0, 1.0, 10.0, 1);
    m_stretch->setToolTip(u"How much the asinh and logarithmic curves lift the faint light"_s);
    gf->addRow(u"Stretch"_s, m_stretch);
    m_gamma = spin(1.0, 4.0, 0.1, 2.2);
    gf->addRow(u"Gamma"_s, m_gamma);
    m_saturation = spin(0.0, 3.0, 0.1, 1.0);
    gf->addRow(u"Saturation"_s, m_saturation);

    m_more = new QToolButton;
    m_more->setText(u"More settings"_s);
    m_more->setCheckable(true);
    m_more->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_more->setArrowType(Qt::RightArrow);
    m_more->setAutoRaise(true);
    m_more->setToolTip(u"The particles' size and what their light measures, absorption, a "
                       u"map's slab and range, the tone curve"_s);
    connect(m_more, &QToolButton::toggled, this, [this](bool open) {
        m_more->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        syncRows();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(style);
    layout->addWidget(m_projectedBox);
    layout->addWidget(particles);
    layout->addWidget(grade);
    layout->addWidget(m_more);
    // Narrow enough for a viewer's sidebar: the fields shrink to the space
    // there is, the menus too (their items stay whole in the list).
    for (QGroupBox *box : { style, m_projectedBox, particles, grade }) {
        box->setFlat(true);   // a title over its rows, no frame inside the host's own
        if (auto *form = qobject_cast<QFormLayout *>(box->layout())) {
            form->setContentsMargins(0, 4, 0, 4);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        }
    }
    for (QComboBox *combo : findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(4);
    }
    for (QAbstractSpinBox *box : findChildren<QAbstractSpinBox *>()) {
        box->setMinimumWidth(44);
        box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    // What changes the particles; what only grades them.
    const auto particles_ = [this]() {
        syncRows();
        emit particlesChanged();
    };
    connect(m_mode, &QComboBox::currentIndexChanged, this, particles_);
    connect(m_colourBy, &QComboBox::currentIndexChanged, this, particles_);
    connect(m_size, &QComboBox::currentIndexChanged, this, particles_);
    connect(m_light, &QComboBox::currentIndexChanged, this, particles_);
    connect(m_quantity, &QComboBox::currentIndexChanged, this, particles_);
    connect(m_lightLog, &QCheckBox::toggled, this, particles_);
    connect(m_neighbours, &QSpinBox::valueChanged, this, particles_);
    for (QDoubleSpinBox *box : { m_sizeScale, m_contrast, m_absorption, m_slab })
        connect(box, &QDoubleSpinBox::valueChanged, this, particles_);
    const auto tone_ = [this]() { emit toneChanged(); };
    connect(m_map, &QComboBox::currentIndexChanged, this, tone_);
    connect(m_mapLog, &QCheckBox::toggled, this, tone_);
    connect(m_curve, &QComboBox::currentIndexChanged, this, tone_);
    for (QDoubleSpinBox *box : { m_exposureEv, m_stretch, m_gamma, m_saturation, m_decades })
        connect(box, &QDoubleSpinBox::valueChanged, this, tone_);
    syncRows();
}

void RaytraceSettingsPanel::setRowVisible(QWidget *field, bool visible)
{
    // Hidden, not greyed: the style draws a disabled field much as an enabled one.
    if (auto *form = qobject_cast<QFormLayout *>(field->parentWidget()->layout()))
        form->setRowVisible(field, visible);
}

void RaytraceSettingsPanel::syncRows()
{
    const bool map = projected();
    const bool more = m_more && m_more->isChecked();
    m_projectedBox->setVisible(map);
    // A map has no absorption, and its brightness is the log of the density:
    // the light's curve does not apply. And the finer settings wait behind
    // "More settings".
    for (QWidget *w : std::initializer_list<QWidget *>{ m_absorption, m_quantity, m_curve,
                                                        m_stretch, m_gamma, m_saturation })
        setRowVisible(w, !map && more);
    for (QWidget *w : std::initializer_list<QWidget *>{ m_size, m_slab, m_decades })
        setRowVisible(w, more);
    if (m_particlesForm && m_sizeRow)
        m_particlesForm->setRowVisible(m_sizeRow, more);
    const QString by = m_colourBy->currentData().toString();
    m_map->setEnabled(by != u"colour"_s);
    m_mapLog->setEnabled(by == u"velocity_dispersion"_s);
    m_neighbours->setEnabled(m_size->currentData().toString().isEmpty());
    m_lightLog->setEnabled(!m_light->currentData().toString().isEmpty());
}

bool RaytraceSettingsPanel::projected() const
{
    return m_mode->currentData().toString() == u"projected"_s;
}

double RaytraceSettingsPanel::absorption() const
{
    return projected() ? 0.0 : m_absorption->value();
}

QJsonObject RaytraceSettingsPanel::fields() const
{
    QJsonObject f;
    const QString size = m_size->currentData().toString();
    if (!size.isEmpty())
        f.insert(u"radius_field"_s, size);
    f.insert(u"radius_scale"_s, m_sizeScale->value());
    f.insert(u"neighbours"_s, m_neighbours->value());
    const QString light = m_light->currentData().toString();
    if (!light.isEmpty()) {
        f.insert(u"intensity_field"_s, light);
        f.insert(u"intensity_log10"_s, m_lightLog->isChecked());
    }
    f.insert(u"intensity_power"_s, m_contrast->value());
    f.insert(u"quantity"_s, m_quantity->currentData().toString());
    f.insert(u"absorption"_s, m_absorption->value());
    if (projected()) {
        f.insert(u"mode"_s, u"projected"_s);
        const QString by = m_colourBy->currentData().toString();
        f.insert(u"colour_by"_s, by);
        if (m_slab->value() > 0.0)
            f.insert(u"slab"_s, m_slab->value());
        // The view's own colours for its column; the map chosen here otherwise.
        if (by != u"colour"_s)
            f.insert(u"colour"_s, QJsonObject{ { u"lut"_s, m_map->currentData().toString() },
                                              { u"log"_s, m_mapLog->isChecked() } });
    }
    return f;
}

QJsonObject RaytraceSettingsPanel::tone(double base) const
{
    QJsonObject t{ { u"curve"_s, m_curve->currentData().toString() },
                   { u"stretch"_s, m_stretch->value() },
                   { u"gamma"_s, m_gamma->value() },
                   { u"saturation"_s, m_saturation->value() },
                   { u"ev"_s, m_exposureEv->value() } };
    if (projected() && m_decades->value() > 0.0)
        t.insert(u"decades"_s, m_decades->value());
    if (base > 0.0)
        t.insert(u"exposure"_s, base);
    return t;
}

ParticleGLView::Tone RaytraceSettingsPanel::gpuTone(const QJsonObject &viewColour) const
{
    ParticleGLView::Tone t;
    t.curve = m_curve->currentData().toString();
    t.stretch = m_stretch->value();
    t.gamma = m_gamma->value();
    t.saturation = m_saturation->value();
    t.ev = m_exposureEv->value();
    t.decades = m_decades->value();
    if (projected()) {
        const QString by = m_colourBy->currentData().toString();
        if (m_slab->value() > 0.0)
            t.slab = m_slab->value();
        if (by == u"colour"_s && !viewColour.value(u"points"_s).isArray()) {
            // The view draws in one colour: by the density, as the backend does.
            t.hue = ParticleGLView::Hue::Density;
            t.lut = namedLut(viewColour.value(u"lut"_s).toString(u"Inferno"_s));
        } else if (by == u"colour"_s) {
            t.hue = ParticleGLView::Hue::ViewColours;
            t.hueLog = viewColour.value(u"log"_s).toBool();
            t.hueMin = viewColour.value(u"min"_s).toDouble();
            t.hueMax = viewColour.value(u"max"_s).toDouble(1.0);
            t.lut = pointsLut(viewColour.value(u"points"_s).toArray(), t.hueMin, t.hueMax,
                              t.hueLog);
            t.barTitle = viewColour.value(u"field"_s).toString();
        } else {
            t.hue = by == u"velocity_dispersion"_s ? ParticleGLView::Hue::NamedMap
                                                     : ParticleGLView::Hue::Density;
            t.hueLog = m_mapLog->isChecked();
            t.lut = namedLut(m_map->currentData().toString());
            if (t.hue == ParticleGLView::Hue::NamedMap)
                t.barTitle = u"Velocity dispersion"_s;
        }
    } else if (viewColour.value(u"points"_s).isArray()) {
        // A 3-D render's colours are the view's: its colour bar too.
        t.hueLog = viewColour.value(u"log"_s).toBool();
        t.hueMin = viewColour.value(u"min"_s).toDouble();
        t.hueMax = viewColour.value(u"max"_s).toDouble(1.0);
        t.lut = pointsLut(viewColour.value(u"points"_s).toArray(), t.hueMin, t.hueMax, t.hueLog);
        t.barTitle = viewColour.value(u"field"_s).toString();
    }
    return t;
}

QString RaytraceSettingsPanel::particlesKey(const QJsonObject &request)
{
    QJsonObject key;
    for (const QString &k : { u"dataset_id"_s, u"x"_s, u"y"_s, u"z"_s, u"sky"_s, u"filters"_s,
                              u"radius_field"_s, u"radius_scale"_s, u"neighbours"_s,
                              u"intensity_field"_s, u"intensity_log10"_s, u"intensity_power"_s,
                              u"quantity"_s, u"mode"_s, u"colour_by"_s })
        if (request.contains(k))
            key.insert(k, request.value(k));
    if (request.value(u"mode"_s).toString() != u"projected"_s)
        key.insert(u"colour"_s, request.value(u"colour"_s));
    else if (request.value(u"colour_by"_s).toString() == u"colour"_s)
        key.insert(u"colour_field"_s, request.value(u"colour"_s).toObject().value(u"field"_s));
    return QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
}

QJsonObject RaytraceSettingsPanel::settings() const
{
    return { { u"mode"_s, m_mode->currentIndex() },     { u"colour_by"_s, m_colourBy->currentIndex() },
             { u"map"_s, m_map->currentIndex() },       { u"map_log"_s, m_mapLog->isChecked() },
             { u"slab"_s, m_slab->value() },            { u"decades"_s, m_decades->value() },
             { u"size"_s, m_size->currentIndex() },     { u"neighbours"_s, m_neighbours->value() },
             { u"scale"_s, m_sizeScale->value() },      { u"light"_s, m_light->currentIndex() },
             { u"light_log"_s, m_lightLog->isChecked() }, { u"contrast"_s, m_contrast->value() },
             { u"quantity"_s, m_quantity->currentIndex() },
             { u"absorption"_s, m_absorption->value() }, { u"ev"_s, m_exposureEv->value() },
             { u"curve"_s, m_curve->currentIndex() },   { u"stretch"_s, m_stretch->value() },
             { u"gamma"_s, m_gamma->value() },          { u"saturation"_s, m_saturation->value() },
             { u"more"_s, m_more->isChecked() } };
}

void RaytraceSettingsPanel::setSettings(const QJsonObject &s)
{
    {
        // One change for all of them, not one per control.
        const QList<QWidget *> all{ m_mode, m_colourBy, m_map, m_mapLog, m_slab, m_decades,
                                    m_size, m_neighbours, m_sizeScale, m_light, m_lightLog,
                                    m_contrast, m_quantity, m_absorption, m_exposureEv,
                                    m_curve, m_stretch, m_gamma, m_saturation };
        std::vector<std::unique_ptr<QSignalBlocker>> blocks;
        for (QWidget *w : all)
            blocks.push_back(std::make_unique<QSignalBlocker>(w));
        m_mode->setCurrentIndex(s.value(u"mode"_s).toInt());
        m_colourBy->setCurrentIndex(s.value(u"colour_by"_s).toInt());
        m_map->setCurrentIndex(s.value(u"map"_s).toInt());
        m_mapLog->setChecked(s.value(u"map_log"_s).toBool(true));
        m_slab->setValue(s.value(u"slab"_s).toDouble());
        m_decades->setValue(s.value(u"decades"_s).toDouble());
        m_size->setCurrentIndex(s.value(u"size"_s).toInt());
        m_neighbours->setValue(s.value(u"neighbours"_s).toInt(32));
        m_sizeScale->setValue(s.value(u"scale"_s).toDouble(1.0));
        m_light->setCurrentIndex(s.value(u"light"_s).toInt());
        m_lightLog->setChecked(s.value(u"light_log"_s).toBool());
        m_contrast->setValue(s.value(u"contrast"_s).toDouble(1.0));
        m_quantity->setCurrentIndex(s.value(u"quantity"_s).toInt());
        m_absorption->setValue(s.value(u"absorption"_s).toDouble());
        m_exposureEv->setValue(s.value(u"ev"_s).toDouble());
        m_curve->setCurrentIndex(s.value(u"curve"_s).toInt());
        m_stretch->setValue(s.value(u"stretch"_s).toDouble(10.0));
        m_gamma->setValue(s.value(u"gamma"_s).toDouble(2.2));
        m_saturation->setValue(s.value(u"saturation"_s).toDouble(1.0));
        m_more->setChecked(s.value(u"more"_s).toBool());
        m_more->setArrowType(m_more->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    }
    syncRows();
    emit particlesChanged();
}
