#include "NoiseRegionDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

NoiseRegionDialog::NoiseRegionDialog(int width, int height, int depth, QWidget *parent)
    : QDialog(parent)
    , m_width(width)
    , m_height(height)
    , m_depth(depth)
{
    setWindowTitle(u"Noise Estimation — Select Region"_s);
    setModal(true);

    const int xMax = std::max(0, width  - 1);
    const int yMax = std::max(0, height - 1);
    const int zMax = std::max(0, depth  - 1);

    auto makeSpinBox = [&](int lo, int hi, int val) -> QSpinBox * {
        auto *sb = new QSpinBox(this);
        sb->setRange(lo, hi);
        sb->setValue(val);
        return sb;
    };

    // ── Spatial group ─────────────────────────────────────────────────────
    auto *spatialGroup = new QGroupBox(u"Spatial region (pixels)"_s, this);
    auto *spatialForm  = new QFormLayout(spatialGroup);

    m_x0Spin = makeSpinBox(0, xMax, 0);
    m_x1Spin = makeSpinBox(0, xMax, xMax);
    m_y0Spin = makeSpinBox(0, yMax, 0);
    m_y1Spin = makeSpinBox(0, yMax, yMax);

    spatialForm->addRow(u"x min:"_s, m_x0Spin);
    spatialForm->addRow(u"x max:"_s, m_x1Spin);
    spatialForm->addRow(u"y min:"_s, m_y0Spin);
    spatialForm->addRow(u"y max:"_s, m_y1Spin);

    // ── Spectral group ────────────────────────────────────────────────────
    auto *spectralGroup = new QGroupBox(u"Spectral range (channels)"_s, this);
    auto *spectralForm  = new QFormLayout(spectralGroup);

    m_chStartSpin = makeSpinBox(0, zMax, 0);
    m_chEndSpin   = makeSpinBox(0, zMax, zMax);

    spectralForm->addRow(u"Channel start:"_s, m_chStartSpin);
    spectralForm->addRow(u"Channel end:"_s,   m_chEndSpin);

    // ── Buttons ───────────────────────────────────────────────────────────
    auto *btnBox = new QDialogButtonBox(this);
    m_okBtn = btnBox->addButton(QDialogButtonBox::Ok);
    btnBox->addButton(QDialogButtonBox::Cancel);
    auto *resetBtn = btnBox->addButton(u"Reset to Full Extent"_s, QDialogButtonBox::ResetRole);

    QObject::connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    QObject::connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(resetBtn, &QPushButton::clicked, this, &NoiseRegionDialog::resetToFullExtent);

    // ── Layout ────────────────────────────────────────────────────────────
    auto *root = new QVBoxLayout(this);
    root->addWidget(new QLabel(u"Choose the region used to estimate noise.\n"
                               "Defaults to the full cube extent."_s));
    root->addWidget(spatialGroup);
    root->addWidget(spectralGroup);
    root->addWidget(btnBox);

    // Wire live validation
    const auto validate = [this]() { this->validate(); };
    QObject::connect(m_x0Spin,    &QSpinBox::valueChanged, this, validate);
    QObject::connect(m_x1Spin,    &QSpinBox::valueChanged, this, validate);
    QObject::connect(m_y0Spin,    &QSpinBox::valueChanged, this, validate);
    QObject::connect(m_y1Spin,    &QSpinBox::valueChanged, this, validate);
    QObject::connect(m_chStartSpin, &QSpinBox::valueChanged, this, validate);
    QObject::connect(m_chEndSpin,   &QSpinBox::valueChanged, this, validate);

    validate();
}

void NoiseRegionDialog::validate()
{
    const bool ok = m_x0Spin->value()    <= m_x1Spin->value()
                 && m_y0Spin->value()    <= m_y1Spin->value()
                 && m_chStartSpin->value() <= m_chEndSpin->value();
    m_okBtn->setEnabled(ok);
}

void NoiseRegionDialog::resetToFullExtent()
{
    m_x0Spin->setValue(0);
    m_x1Spin->setValue(std::max(0, m_width  - 1));
    m_y0Spin->setValue(0);
    m_y1Spin->setValue(std::max(0, m_height - 1));
    m_chStartSpin->setValue(0);
    m_chEndSpin->setValue(std::max(0, m_depth - 1));
}

int NoiseRegionDialog::x0()          const { return m_x0Spin->value(); }
int NoiseRegionDialog::x1()          const { return m_x1Spin->value(); }
int NoiseRegionDialog::y0()          const { return m_y0Spin->value(); }
int NoiseRegionDialog::y1()          const { return m_y1Spin->value(); }
int NoiseRegionDialog::channelStart() const { return m_chStartSpin->value(); }
int NoiseRegionDialog::channelEnd()   const { return m_chEndSpin->value(); }
