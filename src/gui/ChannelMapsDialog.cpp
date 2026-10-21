#include "ChannelMapsDialog.h"

#include "CubeUiAssembler.h"
#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"
#include "vtk/ColorMaps.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

ChannelMapsDialog::ChannelMapsDialog(int cubeDepth, int currentSlice,
                                     const QString &currentLut,
                                     QWidget *parent)
    : QDialog(parent)
    , m_cubeDepth(std::max(1, cubeDepth))
{
    setWindowTitle(u"Channel Maps"_s);
    setModal(true);
    setStyleSheet(ToolDialogStyle::inputCss());

    const int maxCh = m_cubeDepth - 1;

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    root->addWidget(ToolDialogStyle::makeBreadcrumb(this, u"Channel Maps"_s));

    auto *intro = new QLabel(
            u"Display a mosaic of 2-D slice panels at consecutive (or "
            u"strided) velocity channels, all with the same color scale. "
            u"Useful for spotting coherent spatial structures across the "
            u"spectral axis."_s, this);
    intro->setStyleSheet(ToolDialogStyle::helperCss());
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *form = ToolDialogStyle::addSection(root, u"Channel range"_s, this);

    // Channels are numbered from 1 here, as on the channel bar and in every other
    // tool; config() hands the backend its 0-based indices.
    m_startSpin = new QSpinBox(this);
    m_startSpin->setRange(1, maxCh + 1);
    m_startSpin->setValue(std::clamp(currentSlice, 0, maxCh) + 1);
    form->addRow(u"Start channel"_s, m_startSpin);

    m_endSpin = new QSpinBox(this);
    m_endSpin->setRange(1, maxCh + 1);
    m_endSpin->setValue(std::min(m_startSpin->value() + 63, maxCh + 1));
    form->addRow(u"End channel"_s, m_endSpin);

    m_strideSpin = new QSpinBox(this);
    m_strideSpin->setRange(1, std::max(1, maxCh));
    m_strideSpin->setValue(1);
    form->addRow(u"Stride"_s, m_strideSpin);

    auto *gridForm = ToolDialogStyle::addSection(root, u"Layout"_s, this);

    m_colsSpin = new QSpinBox(this);
    m_colsSpin->setRange(1, 16);
    m_colsSpin->setValue(8);
    gridForm->addRow(u"Columns"_s, m_colsSpin);

    m_lutCombo = new QComboBox(this);
    m_lutCombo->setIconSize(QSize(48, 12));
    const auto names = ColorMaps::GetColorMapNames();
    for (const auto &stdName : names) {
        const QString qName = QString::fromStdString(stdName);
        m_lutCombo->addItem(
                QIcon(CubeUiAssembler::buildLutPreview(qName, 48, 12)),
                qName);
    }
    if (!currentLut.isEmpty())
        m_lutCombo->setCurrentText(currentLut);
    gridForm->addRow(u"Color map"_s, m_lutCombo);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(ToolDialogStyle::helperCss());
    m_summaryLabel->setWordWrap(true);
    root->addWidget(m_summaryLabel);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, this);
    auto *okBtn    = VisivoTheme::makePrimaryButton(u"Generate"_s, this);
    okBtn->setDefault(true);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(okBtn);
    root->addLayout(btnRow);

    QObject::connect(okBtn,    &QPushButton::clicked, this, &QDialog::accept);
    QObject::connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    const auto onAny = [this]() { this->updateSummary(); };
    QObject::connect(m_startSpin,  &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_endSpin,    &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_strideSpin, &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_colsSpin,   &QSpinBox::valueChanged, this, onAny);

    updateSummary();
}

ChannelMapsConfig ChannelMapsDialog::config() const
{
    ChannelMapsConfig cfg;
    cfg.channelStart = m_startSpin->value() - 1;   // 0-based from here on
    cfg.channelEnd   = m_endSpin->value() - 1;
    cfg.stride       = std::max(1, m_strideSpin->value());
    cfg.columns      = std::max(1, m_colsSpin->value());
    cfg.lutName      = m_lutCombo->currentText();
    cfg.sharedRange  = true;
    return cfg;
}

void ChannelMapsDialog::updateSummary()
{
    const int s = m_startSpin->value();
    const int e = m_endSpin->value();
    const int stride = std::max(1, m_strideSpin->value());
    const int cols   = std::max(1, m_colsSpin->value());
    if (s > e) {
        m_summaryLabel->setText(u"⚠ Start must be ≤ End."_s);
        return;
    }
    const int nPanels = (e - s) / stride + 1;
    const int nRows   = static_cast<int>(std::ceil(
            static_cast<double>(nPanels) / cols));
    m_summaryLabel->setText(
            u"%1 panels (%2 columns × %3 rows)."_s
            .arg(nPanels).arg(cols).arg(nRows));
}
