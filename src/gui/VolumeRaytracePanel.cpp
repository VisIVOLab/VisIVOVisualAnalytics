#include "VolumeRaytracePanel.h"

#include "ColormapIcon.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <initializer_list>

using namespace Qt::Literals::StringLiterals;

namespace {

// The colour's source, as the "Colour by" items hold it: the light's own
// values, each channel's velocity, or a VBT field's index (0 and up).
constexpr int kOwnValues = -1;
constexpr int kVelocity = -2;

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

VolumeRaytracePanel::VolumeRaytracePanel(QWidget *parent) : QWidget(parent)
{
    // The settings one changes, then the rest under "More settings".
    auto *basic = new QWidget;
    auto *bf = new QFormLayout(basic);
    m_mode = new QComboBox;
    m_mode->addItem(u"3-D light"_s, u"light"_s);
    m_mode->addItem(u"Projected map"_s, u"projected"_s);
    m_mode->setToolTip(u"3-D light: the cube emits and absorbs light, in perspective, graded "
                       u"by a tone curve: its faint emission and its bright cores both show.\n"
                       u"Projected map: the column along each line of sight, without "
                       u"perspective, the brightness its logarithm."_s);
    bf->addRow(u"Mode"_s, m_mode);
    m_lightLog = new QCheckBox(u"Values are log₁₀"_s);
    m_lightLog->setToolTip(u"The cube holds the logarithm of the quantity (a log density): "
                           u"its light is 10 to the value"_s);
    bf->addRow(u"Light"_s, m_lightLog);
    m_floor = new QCheckBox(u"Dark below the threshold"_s);
    m_floor->setToolTip(u"Values below the 3-D view's threshold emit nothing. A ray sums every "
                        u"channel it crosses: a radio cube's noise, a little in each, adds up "
                        u"to a veil over the source. A simulation's faint gas is light, not "
                        u"noise: leave it off there."_s);
    bf->addRow(QString(), m_floor);
    m_contrast = spin(0.25, 4.0, 0.1, 1.0);
    m_contrast->setToolTip(u"The light raised to this power. Above 1 the faint, diffuse "
                           u"emission fades and the dense structure stands out (2: a gas's "
                           u"density squared, as an emissivity ∝ ρ² shows it)."_s);
    bf->addRow(u"Contrast"_s, m_contrast);
    m_colour = new QComboBox;
    m_colour->addItem(u"Its own values"_s, kOwnValues);
    m_colour->setToolTip(u"The colour: of the light's own quantity, of another field of the "
                         u"volume (a density as the light, a temperature as the colour), or "
                         u"of each channel's velocity (a spectral cube's motions: in a map "
                         u"seen along the spectrum, its mean velocity)"_s);
    bf->addRow(u"Colour by"_s, m_colour);
    m_map = new QComboBox;
    m_map->setIconSize(QSize(28, 10));
    for (const QString &name : { u"Inferno"_s, u"Magma"_s, u"Plasma"_s, u"Viridis"_s,
                                 u"Cividis"_s, u"Glow"_s, u"Temperature"_s, u"Coolwarm"_s,
                                 u"Gray"_s })
        m_map->addItem(makeColormapIcon(name.toStdString()), name, name);
    bf->addRow(u"Map"_s, m_map);
    m_mapLog = new QCheckBox(u"Log scale"_s);
    m_mapLog->setChecked(true);
    m_mapLog->setToolTip(u"The colour's quantity on a log scale"_s);
    bf->addRow(QString(), m_mapLog);

    m_exposureEv = spin(-12.0, 12.0, 0.5, 0.0);
    m_exposureEv->setSuffix(u" EV"_s);
    m_exposureEv->setToolTip(u"Relative to the automatic exposure: +1 doubles the light."_s);
    bf->addRow(u"Exposure"_s, m_exposureEv);

    m_advanced = new QWidget;
    auto *gf = new QFormLayout(m_advanced);
    m_absorption = spin(0.0, 20.0, 0.25, 0.0);
    m_absorption->setToolTip(u"Grey absorption, as the optical depth where the light is "
                             u"brightest. 0: emission only; 1 or more: dense regions hide "
                             u"what lies behind them."_s);
    gf->addRow(u"Absorption"_s, m_absorption);
    m_quality = spin(0.5, 8.0, 0.5, 2.0, 1);
    m_quality->setSuffix(u" / voxel"_s);
    m_quality->setToolTip(u"Samples along each ray per voxel: more, finer and slower"_s);
    gf->addRow(u"Samples"_s, m_quality);
    m_slab = spin(0.0, 1.0e9, 1.0, 0.0);
    m_slab->setSpecialValueText(u"Everything"_s);
    m_slab->setToolTip(u"Only what lies within this thickness of the focal plane, in voxels"_s);
    gf->addRow(u"Slab"_s, m_slab);
    m_decades = spin(0.0, 12.0, 0.5, 0.0);
    m_decades->setSpecialValueText(u"Automatic"_s);
    m_decades->setSuffix(u" dec"_s);
    m_decades->setToolTip(u"The decades of column the brightness spans, down from the "
                          u"brightest"_s);
    gf->addRow(u"Range"_s, m_decades);
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
    m_more->setToolTip(u"Absorption, samples, a map's slab and range, the tone curve"_s);
    connect(m_more, &QToolButton::toggled, this, [this](bool open) {
        m_more->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        syncRows();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    for (QFormLayout *form : { bf, gf }) {
        form->setContentsMargins(0, 2, 0, 2);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    layout->addWidget(basic);
    layout->addWidget(m_more);
    layout->addWidget(m_advanced);
    // Narrow enough for the viewer's sidebar.
    for (QComboBox *combo : findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(4);
    }
    for (QAbstractSpinBox *box : findChildren<QAbstractSpinBox *>()) {
        box->setMinimumWidth(44);
        box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    // What changes the grid; what only draws it again.
    const auto grid = [this]() {
        syncRows();
        emit gridChanged();
    };
    connect(m_mode, &QComboBox::currentIndexChanged, this, grid);
    connect(m_colour, &QComboBox::currentIndexChanged, this, [this, grid]() {
        // Approaching blue, receding red.
        if (m_colour->currentData().toInt() == kVelocity)
            setMap(u"Coolwarm"_s);
        grid();
    });
    connect(m_map, &QComboBox::currentIndexChanged, this, grid);
    connect(m_mapLog, &QCheckBox::toggled, this, grid);
    connect(m_lightLog, &QCheckBox::toggled, this, grid);
    connect(m_floor, &QCheckBox::toggled, this, grid);
    connect(m_contrast, &QDoubleSpinBox::valueChanged, this, grid);
    const auto tone = [this]() { emit toneChanged(); };
    connect(m_curve, &QComboBox::currentIndexChanged, this, tone);
    for (QDoubleSpinBox *box : { m_exposureEv, m_absorption, m_quality, m_slab, m_decades,
                                 m_stretch, m_gamma, m_saturation })
        connect(box, &QDoubleSpinBox::valueChanged, this, tone);
    syncRows();
}

void VolumeRaytracePanel::setFields(const QList<QPair<QString, int>> &fields)
{
    const QSignalBlocker block(m_colour);
    const int chosen = m_colour->currentData().toInt();
    for (int i = m_colour->count() - 1; i >= 0; --i)
        if (m_colour->itemData(i).toInt() >= 0)
            m_colour->removeItem(i);
    for (const auto &[name, index] : fields)
        m_colour->addItem(name, index);
    const int at = m_colour->findData(chosen);
    m_colour->setCurrentIndex(at >= 0 ? at : 0);
    syncRows();
}

void VolumeRaytracePanel::setVelocity(bool available)
{
    const QSignalBlocker block(m_colour);
    const int at = m_colour->findData(kVelocity);
    if (available && at < 0)
        m_colour->insertItem(1, u"Velocity"_s, kVelocity);
    else if (!available && at >= 0)
        m_colour->removeItem(at);   // the current item moves to the own values
    syncRows();
}

void VolumeRaytracePanel::setMap(const QString &name)
{
    const int at = m_map->findText(name, Qt::MatchFixedString);
    if (at < 0)
        return;
    const QSignalBlocker block(m_map);
    m_map->setCurrentIndex(at);
}

bool VolumeRaytracePanel::thresholdDarkens() const
{
    return m_floor->isChecked();
}

void VolumeRaytracePanel::setThresholdDarkens(bool on)
{
    const QSignalBlocker block(m_floor);
    m_floor->setChecked(on);
}

void VolumeRaytracePanel::setRowVisible(QWidget *field, bool visible)
{
    if (auto *form = qobject_cast<QFormLayout *>(field->parentWidget()->layout()))
        form->setRowVisible(field, visible);
}

void VolumeRaytracePanel::syncRows()
{
    const bool map = projected();
    const bool more = m_more && m_more->isChecked();
    // A single-field cube has no colour to choose; a velocity no log scale.
    setRowVisible(m_colour, m_colour->count() > 1);
    setRowVisible(m_mapLog, m_colour->currentData().toInt() != kVelocity);
    // A map has no absorption, and its brightness is the log of the column:
    // the light's curve does not apply.
    m_advanced->setVisible(more);
    for (QWidget *w : std::initializer_list<QWidget *>{ m_absorption, m_curve, m_stretch,
                                                        m_gamma, m_saturation })
        setRowVisible(w, !map);
    for (QWidget *w : std::initializer_list<QWidget *>{ m_slab, m_decades })
        setRowVisible(w, map);
}

bool VolumeRaytracePanel::projected() const
{
    return m_mode->currentData().toString() == u"projected"_s;
}

QJsonObject VolumeRaytracePanel::fields() const
{
    QJsonObject f{ { u"mode"_s, m_mode->currentData().toString() },
                   { u"brightness_log10"_s, m_lightLog->isChecked() },
                   { u"brightness_power"_s, m_contrast->value() },
                   { u"absorption"_s, projected() ? 0.0 : m_absorption->value() },
                   { u"quality"_s, m_quality->value() },
                   { u"colour"_s, QJsonObject{ { u"lut"_s, m_map->currentData().toString() },
                                               { u"log"_s, m_mapLog->isChecked() } } } };
    const int field = m_colour->currentData().toInt();
    if (field >= 0) {
        f.insert(u"colour_by"_s, u"colour"_s);
        f.insert(u"colour_field"_s, field);
    } else if (field == kVelocity) {
        f.insert(u"colour_by"_s, u"velocity"_s);
    } else {
        f.insert(u"colour_by"_s, u"density"_s);
    }
    if (projected() && m_slab->value() > 0.0)
        f.insert(u"slab"_s, m_slab->value());
    return f;
}

QJsonObject VolumeRaytracePanel::tone(double base) const
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

ParticleGLView::Tone VolumeRaytracePanel::gpuTone(const std::vector<QVector3D> &lut, double lo,
                                                  double hi, bool log,
                                                  const QString &brightnessName) const
{
    ParticleGLView::Tone t;
    t.curve = m_curve->currentData().toString();
    t.stretch = m_stretch->value();
    t.gamma = m_gamma->value();
    t.saturation = m_saturation->value();
    t.ev = m_exposureEv->value();
    t.decades = m_decades->value();
    t.absorption = projected() ? 0.0 : m_absorption->value();
    t.quality = m_quality->value();
    t.lut = lut;
    t.hueMin = lo;
    t.hueMax = hi;
    t.hueLog = log;
    const int source = m_colour->currentData().toInt();
    const bool byField = source != kOwnValues;
    t.barTitle = source == kVelocity ? u"km/s"_s
            : byField               ? m_colour->currentText()
                                    : brightnessName;
    if (projected()) {
        t.slab = m_slab->value();
        // The column's own map, or the mean colour index (backend:
        // volume_render._projected_image).
        t.hue = byField ? ParticleGLView::Hue::Index : ParticleGLView::Hue::Density;
        if (!byField)
            t.barTitle.clear();
    }
    return t;
}

QString VolumeRaytracePanel::gridKey(const QJsonObject &request)
{
    QJsonObject key;
    for (const QString &k : { u"dataset_id"_s, u"box"_s, u"brightness_log10"_s,
                              u"brightness_power"_s, u"brightness_floor"_s,
                              u"colour_by"_s, u"colour_field"_s,
                              u"colour"_s, u"mode"_s, u"max_voxels"_s })
        if (request.contains(k))
            key.insert(k, request.value(k));
    return QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
}
