#include "ContourSettingsPanel.h"

#include "ColorMaps.h"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;
namespace vc = visivo::contours;

namespace {

QLineEdit *numberEdit(QWidget *parent)
{
    auto *edit = new QLineEdit(parent);
    edit->setValidator(new QDoubleValidator(edit));
    return edit;
}

double numberOf(const QLineEdit *edit)
{
    return edit ? edit->text().toDouble() : 0.0;
}

QString textOf(double v)
{
    return QString::number(v, 'g', 9);
}

}   // namespace

ContourSettingsPanel::ContourSettingsPanel(QWidget *parent) : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(6);
    m_form = new QFormLayout();
    m_form->setHorizontalSpacing(8);
    m_form->setVerticalSpacing(4);
    outer->addLayout(m_form);

    m_levelMode = new QComboBox(this);
    m_levelMode->addItem(u"Count, Lower to Upper"_s);
    m_levelMode->addItem(u"n × σ of the noise"_s);
    m_levelMode->addItem(u"Logarithmic, Lower to Upper"_s);
    m_levelMode->addItem(u"List"_s);
    m_levelMode->setToolTip(u"How the levels are chosen."_s);
    m_form->addRow(u"Levels"_s, m_levelMode);

    m_count = new QSpinBox(this);
    m_count->setRange(1, 200);
    m_count->setToolTip(u"How many levels, Lower and Upper included."_s);
    m_form->addRow(u"Count"_s, m_count.data());
    m_lower = numberEdit(this);
    m_form->addRow(u"Lower"_s, m_lower.data());
    m_upper = numberEdit(this);
    m_form->addRow(u"Upper"_s, m_upper.data());

    m_sigmaRow = new QWidget(this);
    auto *sigmaLay = new QHBoxLayout(m_sigmaRow);
    sigmaLay->setContentsMargins(0, 0, 0, 0);
    sigmaLay->setSpacing(4);
    m_sigma = numberEdit(m_sigmaRow);
    m_sigma->setToolTip(u"The noise, in the unit of the data: levels are multiples of it."_s);
    auto *estimate = new QPushButton(u"Estimate"_s, m_sigmaRow);
    estimate->setToolTip(u"Measure the noise of this image (1.4826 × the median absolute "
                         u"deviation, which the bright sources hardly move)."_s);
    sigmaLay->addWidget(m_sigma.data(), 1);
    sigmaLay->addWidget(estimate);
    m_form->addRow(u"Noise σ"_s, m_sigmaRow.data());
    m_multiples = new QLineEdit(this);
    m_multiples->setToolTip(u"The multiples of σ to draw, separated by commas."_s);
    m_form->addRow(u"Multiples"_s, m_multiples.data());
    m_negative = new QCheckBox(u"Also -n σ"_s, this);
    m_negative->setToolTip(u"Draw the negative levels as well (dashed when Dash negative is on)."_s);
    m_form->addRow(QString(), m_negative.data());
    m_list = new QLineEdit(this);
    m_list->setToolTip(u"The levels, in the unit of the data, separated by commas."_s);
    m_form->addRow(u"Levels"_s, m_list.data());

    m_smoothRow = new QWidget(this);
    auto *smoothLay = new QHBoxLayout(m_smoothRow);
    smoothLay->setContentsMargins(0, 0, 0, 0);
    smoothLay->setSpacing(4);
    m_smoothing = new QDoubleSpinBox(m_smoothRow);
    m_smoothing->setRange(0.0, 200.0);
    m_smoothing->setDecimals(2);
    m_smoothing->setSingleStep(0.5);
    m_smoothing->setSpecialValueText(u"Off"_s);
    m_smoothing->setToolTip(u"Gaussian width (σ) the pixels are smoothed with before they are "
                            u"contoured. The image itself is not changed."_s);
    m_smoothingUnit = new QComboBox(m_smoothRow);
    m_smoothingUnit->addItem(u"pixels"_s);
    m_smoothingUnit->addItem(u"arcsec"_s);
    smoothLay->addWidget(m_smoothing.data(), 1);
    smoothLay->addWidget(m_smoothingUnit.data());
    m_form->addRow(u"Smooth"_s, m_smoothRow.data());

    m_colourMode = new QComboBox(this);
    m_colourMode->addItem(u"One colour"_s);
    m_colourMode->addItem(u"Colour map over the levels"_s);
    m_colourMode->addItem(u"Colour per level"_s);
    m_form->addRow(u"Colour"_s, m_colourMode.data());
    m_colourButton = new QPushButton(this);
    m_colourButton->setToolTip(u"Pick the colour of every level."_s);
    m_form->addRow(QString(), m_colourButton.data());
    m_colourMap = new QComboBox(this);
    for (const std::string &name : ColorMaps::GetColorMapNames())
        m_colourMap->addItem(QString::fromStdString(name));
    m_form->addRow(u"Map"_s, m_colourMap.data());
    m_perLevel = new QLineEdit(this);
    m_perLevel->setPlaceholderText(u"red, #00ff00, orange, …"_s);
    m_perLevel->setToolTip(u"One colour for each level, lowest first, separated by commas; "
                           u"they repeat when there are more levels than colours."_s);
    m_form->addRow(u"Colours"_s, m_perLevel.data());

    m_width = new QDoubleSpinBox(this);
    m_width->setRange(0.5, 10.0);
    m_width->setSingleStep(0.5);
    m_width->setDecimals(1);
    m_form->addRow(u"Line width"_s, m_width.data());
    m_dash = new QCheckBox(u"Dash negative levels"_s, this);
    m_form->addRow(QString(), m_dash.data());

    m_problem = new QLabel(this);
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(u"QLabel { color: #d9534f; }"_s);
    m_problem->hide();
    outer->addWidget(m_problem.data());
    // What is drawn: the noise and the levels in use, for whoever reads the
    // figure (the typed numbers may be rounded, or come from the image).
    m_levelsInfo = new QLabel(this);
    m_levelsInfo->setWordWrap(true);
    m_levelsInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_levelsInfo->setStyleSheet(u"QLabel { color: palette(placeholder-text); }"_s);
    m_levelsInfo->hide();
    outer->addWidget(m_levelsInfo.data());

    // The panel lives in the Inspector: a combo box as wide as its longest
    // entry (a file name) would push it past the edge.
    for (QComboBox *combo : findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8);
    }

    // Edits commit when finished (text) or at once (the rest).
    connect(m_levelMode, &QComboBox::currentIndexChanged, this, [this] {
        syncRows();
        emitChanged();
    });
    connect(m_colourMode, &QComboBox::currentIndexChanged, this, [this] {
        syncRows();
        emitChanged();
    });
    connect(m_count, &QSpinBox::valueChanged, this, [this] { emitChanged(); });
    connect(m_smoothing, &QDoubleSpinBox::valueChanged, this, [this] { emitChanged(); });
    connect(m_smoothingUnit, &QComboBox::currentIndexChanged, this, [this] { emitChanged(); });
    connect(m_colourMap, &QComboBox::currentIndexChanged, this, [this] { emitChanged(); });
    connect(m_width, &QDoubleSpinBox::valueChanged, this, [this] { emitChanged(); });
    connect(m_negative, &QCheckBox::toggled, this, [this] { emitChanged(); });
    connect(m_dash, &QCheckBox::toggled, this, [this] { emitChanged(); });
    for (QLineEdit *edit : { m_lower.data(), m_upper.data(), m_sigma.data(), m_multiples.data(),
                             m_list.data(), m_perLevel.data() })
        connect(edit, &QLineEdit::editingFinished, this, [this] { emitChanged(); });
    connect(m_colourButton, &QPushButton::clicked, this, [this] { pickColour(); });
    connect(estimate, &QPushButton::clicked, this, &ContourSettingsPanel::estimateRequested);

    setSettings(m_state);
}

void ContourSettingsPanel::setSettings(const vc::Settings &s)
{
    m_state = s;
    const QSignalBlocker b1(m_levelMode), b2(m_count), b3(m_lower), b4(m_upper), b5(m_sigma),
            b6(m_multiples), b7(m_negative), b8(m_list), b9(m_smoothing), b10(m_smoothingUnit),
            b11(m_colourMode), b12(m_colourMap), b13(m_perLevel), b14(m_width), b15(m_dash);
    m_levelMode->setCurrentIndex(int(s.levelMode));
    m_count->setValue(s.count);
    m_lower->setText(textOf(s.lower));
    m_upper->setText(textOf(s.upper));
    m_sigma->setText(s.sigma > 0.0 ? textOf(s.sigma) : QString());
    m_multiples->setText(s.multiples);
    m_negative->setChecked(s.negative);
    m_list->setText(s.list);
    m_smoothing->setValue(s.smoothing);
    m_smoothingUnit->setCurrentIndex(s.smoothingInArcsec ? 1 : 0);
    m_colourMode->setCurrentIndex(int(s.colourMode));
    const int mapIndex = s.colourMap.isEmpty()
                                 ? m_colourMap->findText(QString::fromStdString(ColorMaps::DefaultColorMap))
                                 : m_colourMap->findText(s.colourMap);
    m_colourMap->setCurrentIndex(std::max(0, mapIndex));
    m_perLevel->setText(s.perLevel);
    m_width->setValue(s.lineWidth);
    m_dash->setChecked(s.dashNegative);
    const QColor c(s.colour);
    m_colourButton->setText(c.isValid() ? c.name() : s.colour);
    m_colourButton->setStyleSheet(
            c.isValid() ? u"QPushButton { background-color: %1; color: %2; }"_s
                                  .arg(c.name(), c.lightness() > 128 ? u"#000"_s : u"#fff"_s)
                        : QString());
    syncRows();
    updateLevelsInfo();
}

void ContourSettingsPanel::updateLevelsInfo()
{
    if (!m_levelsInfo)
        return;
    const vc::Settings s = settings();
    const QString text = vc::describeLevels(s, vc::levels(s));
    m_levelsInfo->setText(text);
    m_levelsInfo->setVisible(!text.isEmpty());
}

vc::Settings ContourSettingsPanel::settings() const
{
    vc::Settings s = m_state;   // the colour is held by the button, not an edit
    s.levelMode = vc::LevelMode(std::clamp(m_levelMode->currentIndex(), 0, 3));
    s.count = m_count->value();
    s.lower = numberOf(m_lower);
    s.upper = numberOf(m_upper);
    s.sigma = numberOf(m_sigma);
    s.multiples = m_multiples->text();
    s.negative = m_negative->isChecked();
    s.list = m_list->text();
    s.smoothing = m_smoothing->value();
    s.smoothingInArcsec = m_smoothingUnit->currentIndex() == 1;
    s.colourMode = vc::ColourMode(std::clamp(m_colourMode->currentIndex(), 0, 2));
    s.colourMap = m_colourMap->currentText();
    s.perLevel = m_perLevel->text();
    s.lineWidth = m_width->value();
    s.dashNegative = m_dash->isChecked();
    return s;
}

void ContourSettingsPanel::setProblem(const QString &text)
{
    m_problem->setText(text);
    m_problem->setVisible(!text.isEmpty());
}

void ContourSettingsPanel::syncRows()
{
    const int mode = m_levelMode->currentIndex();
    const bool count = mode == int(vc::LevelMode::Count), log = mode == int(vc::LevelMode::Log),
               sigma = mode == int(vc::LevelMode::Sigma), list = mode == int(vc::LevelMode::List);
    m_form->setRowVisible(m_count, count || log);
    m_form->setRowVisible(m_lower, count || log);
    m_form->setRowVisible(m_upper, count || log);
    m_form->setRowVisible(m_sigmaRow, sigma);
    m_form->setRowVisible(m_multiples, sigma);
    m_form->setRowVisible(m_negative, sigma);
    m_form->setRowVisible(m_list, list);
    const int colour = m_colourMode->currentIndex();
    m_form->setRowVisible(m_colourButton, colour == int(vc::ColourMode::Single));
    m_form->setRowVisible(m_colourMap, colour == int(vc::ColourMode::Map));
    m_form->setRowVisible(m_perLevel, colour == int(vc::ColourMode::PerLevel));
}

void ContourSettingsPanel::pickColour()
{
    const QColor start(m_state.colour);
    const QColor picked = QColorDialog::getColor(start.isValid() ? start : QColor(255, 102, 51),
                                                 this, u"Contour colour"_s);
    if (!picked.isValid())
        return;
    m_state.colour = picked.name();
    m_colourButton->setText(picked.name());
    m_colourButton->setStyleSheet(u"QPushButton { background-color: %1; color: %2; }"_s.arg(
            picked.name(), picked.lightness() > 128 ? u"#000"_s : u"#fff"_s));
    emitChanged();
}

void ContourSettingsPanel::emitChanged()
{
    updateLevelsInfo();
    emit changed();
}
