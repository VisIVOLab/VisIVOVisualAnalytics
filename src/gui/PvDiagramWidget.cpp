#include "PvDiagramWidget.h"

#include "ColorMaps.h"
#include "PvPathGeometry.h"
#include "qcustomplot.h"
#include "theme/VisivoTheme.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QLineEdit>
#include <QDoubleValidator>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

// ── Theme accessors ───────────────────────────────────────────────────────────
// Use VisivoTheme tokens so the PV window shares the exact same surface ramp
// as ToolResultDialog (Noise / Stack / Region info dialogs). Hardcoded hex
// codes are deliberately gone — they used to drift from the global theme.
namespace {
QString tBg()         { return QString::fromLatin1(VisivoTheme::kSurface()); }
QString tHeaderBg()   { return QString::fromLatin1(VisivoTheme::kSurfaceContainerLow()); }
QString tBorder()     { return QString::fromLatin1(VisivoTheme::kOutline()); }
QString tTitle()      { return QString::fromLatin1(VisivoTheme::kOnSurface()); }
QString tKey()        { return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()); }
QString tValue()      { return QString::fromLatin1(VisivoTheme::kOnSurface()); }
QString tGroupTitle() { return QString::fromLatin1(VisivoTheme::kPrimary()); }
}

// ─────────────────────────────────────────────────────────────────────────────

PvDiagramWidget::PvDiagramWidget(QWidget *parent)
    : QWidget(parent)
    , plot(new QCustomPlot(this))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowFlag(Qt::Window);
    this->setWindowTitle(u"PV Diagram"_s);
    this->resize(960, 700);
    this->setStyleSheet(u"PvDiagramWidget { background: %1; }"_s.arg(tBg()));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Compact header ────────────────────────────────────────────────────────
    buildHeaderUi();    // creates and inserts m_headerWidget into 'root'
    // (buildHeaderUi adds itself to the layout — see below)

    // Separate call so we can use 'root' cleanly.
    // The header frame is created inside buildHeaderUi and added to this widget.
    // Here we just add the plot below.
    root->addWidget(this->plot, 1);

    // ── Footer: path controls + exports ───────────────────────────────────────
    // The path is edited on the image (drag a vertex, drag the path to move it,
    // Shift-drag to rotate) and the diagram follows; width and the curve shape
    // live here, inline, so changing them never interrupts with a dialog.
    auto *footer = new QFrame(this);
    // By object name: a bare "QFrame" selector also styles the QLabels inside
    // (QLabel is a QFrame) and drew a border over the Width label.
    footer->setObjectName(u"pvDiagramFooter"_s);
    footer->setStyleSheet(u"QFrame#pvDiagramFooter { background: %1; border-top: 1px solid %2; }"
                          u"QCheckBox { color: %3; font-size: 11px; background: transparent; spacing: 6px; }"
                          u"QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid %4;"
                          u" border-radius: 3px; background: %1; }"
                          u"QCheckBox::indicator:checked { background: %5; border-color: %5; }"_s
                                  .arg(tHeaderBg(), tBorder(), tValue(), tKey(), tGroupTitle()));
    auto *footerColumn = new QVBoxLayout(footer);
    footerColumn->setContentsMargins(12, 8, 12, 8);
    footerColumn->setSpacing(6);
    auto *footerLayout = new QHBoxLayout;
    footerLayout->setSpacing(10);
    footerColumn->addLayout(footerLayout);
    auto *rangeLayout = new QHBoxLayout;
    rangeLayout->setSpacing(8);
    footerColumn->addLayout(rangeLayout);

    auto *widthLabel = new QLabel(u"Width"_s, footer);
    widthLabel->setStyleSheet(u"color: %1; font-size: 11px; background: transparent;"_s.arg(tKey()));
    m_widthSpin = new QSpinBox(footer);
    m_widthSpin->setRange(1, 99);
    m_widthSpin->setSuffix(u" px"_s);
    m_widthSpin->setKeyboardTracking(false);
    m_widthSpin->setToolTip(u"Width of the cut across the path, averaged (cube pixels)"_s);
    // The same width on the sky: typing it here picks the nearest whole pixel
    // width (the cut is averaged over whole sample steps).
    m_widthArcsecSpin = new QDoubleSpinBox(footer);
    m_widthArcsecSpin->setRange(0.0, 99999.0);
    m_widthArcsecSpin->setDecimals(2);
    m_widthArcsecSpin->setSuffix(u" arcsec"_s);
    m_widthArcsecSpin->setKeyboardTracking(false);
    m_widthArcsecSpin->setEnabled(false);
    m_widthArcsecSpin->setToolTip(u"The same width on the sky. Type a width in arcsec: the "
                                  u"nearest whole pixel width is used."_s);
    m_smoothCheck = new QCheckBox(u"Smooth curve"_s, footer);
    m_smoothCheck->setToolTip(u"Cut along a smooth curve through the vertices instead of straight segments"_s);
    auto *btnNew = new QPushButton(u"New path"_s, footer);
    btnNew->setToolTip(u"Discard this path and draw a new one on the image"_s);
    footerLayout->addWidget(widthLabel);
    footerLayout->addWidget(m_widthSpin);
    footerLayout->addWidget(m_widthArcsecSpin);
    footerLayout->addWidget(m_smoothCheck);
    footerLayout->addWidget(btnNew);
    footerLayout->addStretch(1);

    auto *buttonBox = new QDialogButtonBox(footer);
    auto *btnFits = new QPushButton(u"Save as FITS\u2026"_s);
    btnFits->setToolTip(u"Save the PV diagram as a 2-D FITS image with WCS (offset × spectral axis) "
                        u"in the Workspace Exports"_s);
    auto *btnSave = new QPushButton(u"Save as PNG\u2026"_s);
    btnSave->setToolTip(u"Save the PV diagram as a PNG image"_s);
    buttonBox->addButton(btnFits, QDialogButtonBox::ActionRole);
    buttonBox->addButton(btnSave, QDialogButtonBox::ActionRole);
    footerLayout->addWidget(buttonBox);

    // ── Second row: the part of the band the cut covers, and the path as a region.
    auto *rangeLabel = new QLabel(u"Range"_s, footer);
    rangeLabel->setStyleSheet(u"color: %1; font-size: 11px; background: transparent;"_s.arg(tKey()));
    m_rangeMode = new QComboBox(footer);
    m_rangeMode->addItem(u"Channels"_s);
    m_rangeMode->addItem(u"Axis values"_s);
    m_rangeMode->setToolTip(u"Cut only this part of the spectral band: channels (counted from 1) "
                            u"or values of the spectral axis shown. Leave a side empty for the "
                            u"end of the band."_s);
    const auto makeBound = [&](const QString &hint) {
        auto *edit = new QLineEdit(footer);
        edit->setPlaceholderText(hint);
        edit->setMaximumWidth(90);
        auto *validator = new QDoubleValidator(edit);
        validator->setNotation(QDoubleValidator::StandardNotation);
        edit->setValidator(validator);
        return edit;
    };
    m_rangeFrom = makeBound(u"from"_s);
    m_rangeTo = makeBound(u"to"_s);
    auto *btnWhole = new QPushButton(u"Whole band"_s, footer);
    btnWhole->setToolTip(u"Clear the range: cut the whole spectral band"_s);
    auto *btnRegion = new QPushButton(u"Path as region\u2026"_s, footer);
    btnRegion->setToolTip(u"Write the path (and, if you wish, the polygon of its width) as "
                          u"regions, in a ds9 or CRTF file"_s);
    auto *btnFromRegion = new QPushButton(u"Path from region"_s, footer);
    btnFromRegion->setToolTip(u"Use the selected line (or polyline) region as the PV path"_s);
    rangeLayout->addWidget(rangeLabel);
    rangeLayout->addWidget(m_rangeMode);
    rangeLayout->addWidget(m_rangeFrom);
    rangeLayout->addWidget(new QLabel(u"\u2013"_s, footer));
    rangeLayout->addWidget(m_rangeTo);
    rangeLayout->addWidget(btnWhole);
    rangeLayout->addStretch(1);
    rangeLayout->addWidget(btnFromRegion);
    rangeLayout->addWidget(btnRegion);

    root->addWidget(footer);
    QObject::connect(btnWhole, &QPushButton::clicked, this, [this]() {
        m_rangeFrom->clear();
        m_rangeTo->clear();
        rangeEdited();
    });
    QObject::connect(m_rangeFrom, &QLineEdit::editingFinished, this, &PvDiagramWidget::rangeEdited);
    QObject::connect(m_rangeTo, &QLineEdit::editingFinished, this, &PvDiagramWidget::rangeEdited);
    QObject::connect(m_rangeMode, &QComboBox::activated, this, [this]() { rangeEdited(); });
    QObject::connect(btnRegion, &QPushButton::clicked, this,
                     &PvDiagramWidget::exportPathAsRegionRequested);
    QObject::connect(btnFromRegion, &QPushButton::clicked, this,
                     &PvDiagramWidget::pathFromRegionRequested);
    QObject::connect(m_widthArcsecSpin, &QDoubleSpinBox::valueChanged, this,
                     [this]() { widthArcsecEdited(); });
    // Return commits the typed width even when it rounds to the value shown
    // (valueChanged then does not fire).
    QObject::connect(m_widthArcsecSpin, &QAbstractSpinBox::editingFinished, this,
                     [this]() { widthArcsecEdited(); });
    QObject::connect(btnSave, &QPushButton::clicked, this, &PvDiagramWidget::savePng);
    QObject::connect(btnFits, &QPushButton::clicked, this, &PvDiagramWidget::saveFitsRequested);
    QObject::connect(btnNew, &QPushButton::clicked, this, &PvDiagramWidget::newPathRequested);
    QObject::connect(m_widthSpin, &QSpinBox::valueChanged, this, &PvDiagramWidget::widthChanged);
    QObject::connect(m_smoothCheck, &QCheckBox::toggled, this, &PvDiagramWidget::smoothChanged);

    // ── QCustomPlot setup ─────────────────────────────────────────────────────
    this->plot->plotLayout()->insertRow(0);
    this->titleElement = new QCPTextElement(this->plot, u"PV Diagram"_s);
    QFont titleFont = this->titleElement->font();
    titleFont.setPointSize(10);
    this->titleElement->setFont(titleFont);
    this->plot->plotLayout()->addElement(0, 0, this->titleElement);

    this->colorMap = new QCPColorMap(this->plot->xAxis, this->plot->yAxis);
    this->colorMap->setInterpolate(false);
    this->plot->axisRect()->setupFullAxesBox(true);
    // Colour bar on the right: the diagram's values have a unit (BUNIT), and a
    // colour map without its scale cannot be read.
    this->colorScale = new QCPColorScale(this->plot);
    this->colorScale->setType(QCPAxis::atRight);
    this->plot->plotLayout()->addElement(1, 1, this->colorScale);
    this->colorMap->setColorScale(this->colorScale);
    auto *marginGroup = new QCPMarginGroup(this->plot);
    this->plot->axisRect()->setMarginGroup(QCP::msBottom | QCP::msTop, marginGroup);
    this->colorScale->setMarginGroup(QCP::msBottom | QCP::msTop, marginGroup);

    // Apply default dark theme.
    applyPlotTheme(true);
}

// ─────────────────────────────────────────────────────────────────────────────
// applyPlotTheme
// ─────────────────────────────────────────────────────────────────────────────

void PvDiagramWidget::applyPlotTheme(bool dark)
{
    if (dark) {
        // Plot background follows the global surface colour so the canvas
        // visually melts into the surrounding window. Axes / labels use the
        // on-surface tokens for consistency with the rest of the app.
        this->plot->setBackground(QColor(tBg()));
        this->plot->axisRect()->setBackground(
                QColor(QString::fromLatin1(VisivoTheme::kBackground())));
        if (this->titleElement)
            this->titleElement->setTextColor(QColor(tTitle()));
        const QColor axisLine(tBorder());
        const QColor subTick(QString::fromLatin1(VisivoTheme::kOutlineVariant()));
        const QColor tickLbl(tValue());
        const QColor axisLbl(QString::fromLatin1(VisivoTheme::kPrimary()));
        QList<QCPAxis *> axes{ this->plot->xAxis, this->plot->xAxis2,
                               this->plot->yAxis, this->plot->yAxis2 };
        if (this->colorScale)
            axes << this->colorScale->axis();
        for (auto *axis : axes) {
            axis->setBasePen(QPen(axisLine));
            axis->setTickPen(QPen(axisLine));
            axis->setSubTickPen(QPen(subTick));
            axis->setTickLabelColor(tickLbl);
            axis->setLabelColor(axisLbl);
        }
    } else {
        this->plot->setBackground(Qt::white);
        this->plot->axisRect()->setBackground(QColor(u"#f8f8f8"_s));
        if (this->titleElement)
            this->titleElement->setTextColor(Qt::black);
        QList<QCPAxis *> axes{ this->plot->xAxis, this->plot->xAxis2,
                               this->plot->yAxis, this->plot->yAxis2 };
        if (this->colorScale)
            axes << this->colorScale->axis();
        for (auto *axis : axes) {
            axis->setBasePen(QPen(Qt::black));
            axis->setTickPen(QPen(Qt::black));
            axis->setSubTickPen(QPen(QColor(u"#888888"_s)));
            axis->setTickLabelColor(Qt::black);
            axis->setLabelColor(Qt::black);
        }
    }
    this->plot->replot();
}

// ─────────────────────────────────────────────────────────────────────────────
// savePng
// ─────────────────────────────────────────────────────────────────────────────

void PvDiagramWidget::savePng()
{
    // Native standalone Save dialog. Make ourselves the key window first
    // so macOS restores focus to this PV window on close — see
    // ProfileWidget::runAppModalSaveDialog for the rationale on why every
    // other (sheet, parented) variant has issues here.
    this->raise();
    this->activateWindow();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    QFileDialog dlg(nullptr, u"Save PV Diagram as PNG"_s);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.setDefaultSuffix(u"png"_s);
    dlg.setNameFilters({ u"PNG image (*.png)"_s, u"All files (*)"_s });
    dlg.selectFile(u"pv_diagram.png"_s);
    dlg.setWindowModality(Qt::ApplicationModal);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString path = dlg.selectedFiles().value(0);
    if (path.isEmpty())
        return;
    this->plot->savePng(path, 0, 0, 2.0, 95, 300);
}

// ─────────────────────────────────────────────────────────────────────────────
// buildHeaderUi — constructs the two-column info panel and adds it to the
// widget's top-level layout (which must already exist).
// ─────────────────────────────────────────────────────────────────────────────

void PvDiagramWidget::buildHeaderUi()
{
    auto *header = new QFrame(this);
    header->setObjectName(u"pvDiagramHeader"_s);
    // Matches ToolResultDialog's header band (header bg + bottom hairline).
    header->setStyleSheet(
            u"QFrame#pvDiagramHeader {"
            u"  background: %1;"
            u"  border-bottom: 1px solid %2;"
            u"}"_s.arg(tHeaderBg(), tBorder()));

    auto *hLayout = new QHBoxLayout(header);
    hLayout->setContentsMargins(14, 10, 14, 10);
    hLayout->setSpacing(12);

    auto makeGroup = [&](const QString &title) -> QFormLayout * {
        auto *box = new QGroupBox(title, header);
        // Same section card chrome as ToolResultDialog: rounded outline,
        // uppercase tracking, brand-blue label.
        box->setStyleSheet(
                u"QGroupBox {"
                u"  background: transparent;"
                u"  border: 1px solid %1;"
                u"  border-radius: 6px;"
                u"  margin-top: 10px;"
                u"  color: %2;"
                u"  font-weight: 700;"
                u"  font-size: 10px;"
                u"  letter-spacing: 0.10em;"
                u"}"
                u"QGroupBox::title {"
                u"  subcontrol-origin: margin;"
                u"  left: 10px;"
                u"  padding: 0 6px;"
                u"  text-transform: uppercase;"
                u"}"_s.arg(tBorder(), tGroupTitle()));

        auto *form = new QFormLayout(box);
        form->setContentsMargins(12, 14, 12, 10);
        form->setHorizontalSpacing(24);
        form->setVerticalSpacing(6);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        hLayout->addWidget(box, 1);
        return form;
    };

    auto makeRow = [&](QFormLayout *form,
                       const QString &key,
                       QLabel *&labelOut) {
        auto *keyLbl = new QLabel(key + u":"_s);
        keyLbl->setStyleSheet(
                u"color: %1; font-size: 11px; background: transparent;"_s.arg(tKey()));
        labelOut = new QLabel(u"-"_s);
        // Mono value font + selectable, same as ToolResultDialog values.
        labelOut->setStyleSheet(
                u"color: %1; font-size: 11px; background: transparent; "
                u"font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"_s
                        .arg(tValue()));
        labelOut->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(keyLbl, labelOut);
    };

    // ── Path group ────────────────────────────────────────────────────────────
    auto *pathForm = makeGroup(u"Path"_s);
    makeRow(pathForm, u"Start"_s,    m_lblStart);
    makeRow(pathForm, u"End"_s,      m_lblEnd);
    makeRow(pathForm, u"Vertices"_s, m_lblVertices);
    makeRow(pathForm, u"Length"_s,   m_lblLength);

    // ── Sampling / Axes group ─────────────────────────────────────────────────
    auto *sampForm = makeGroup(u"Sampling && Axes"_s);
    makeRow(sampForm, u"Width"_s,    m_lblWidth);
    makeRow(sampForm, u"Channels"_s, m_lblChannels);
    makeRow(sampForm, u"Spectral"_s, m_lblSpectral);
    makeRow(sampForm, u"Spatial"_s,  m_lblSpatial);
    makeRow(sampForm, u"Scale"_s,    m_lblScale);

    // Insert the header at position 0 in the widget's root layout.
    qobject_cast<QVBoxLayout *>(this->layout())->insertWidget(0, header);
}

void PvDiagramWidget::updateHeaderUi(const PvPathInfo &info)
{
    if (m_lblStart)    m_lblStart->setText(info.startPoint.isEmpty()
                                          ? u"-"_s : info.startPoint);
    if (m_lblEnd)      m_lblEnd->setText(info.endPoint.isEmpty()
                                        ? u"-"_s : info.endPoint);
    if (m_lblVertices) {
        // Numbered as on the image; the offsets say where each lies on the x axis.
        m_lblVertices->setText(info.vertexOffsetsText.isEmpty()
                                       ? QString::number(info.vertexCount)
                                       : u"%1: %2"_s.arg(info.vertexCount).arg(info.vertexOffsetsText));
        m_lblVertices->setToolTip(info.vertexOffsetsText);
    }
    if (m_lblLength)   m_lblLength->setText(
                               QString::number(info.lengthPixels, 'f', 2) + u" px"_s);

    if (m_lblWidth)    m_lblWidth->setText(pvpath::widthText(info.widthPixels, info.stepArcsec));
    if (m_lblChannels) m_lblChannels->setText(info.channelsText.isEmpty()
                                              ? u"whole band"_s : info.channelsText);
    if (m_lblSpectral) m_lblSpectral->setText(
                               info.spectralAxisLabel.isEmpty()
                               ? u"-"_s : info.spectralAxisLabel);
    if (m_lblSpatial)  m_lblSpatial->setText(
                               info.spatialUnit.isEmpty()
                               ? u"pixels"_s : info.spatialUnit);
    if (m_lblScale) {
        if (info.pixelScaleArcsecPerPixel > 0.0)
            m_lblScale->setText(
                    QString::number(info.pixelScaleArcsecPerPixel, 'g', 6)
                    + u" arcsec/px"_s);
        else
            m_lblScale->setText(u"-"_s);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setPvData
// ─────────────────────────────────────────────────────────────────────────────

void PvDiagramWidget::setPvData(const QVector<double> &positions,
                                const QVector<double> &spectral,
                                const QVector<double> &intensities,
                                int nx, int ny,
                                const QString &xLabel,
                                const QString &yLabel,
                                const QString &title,
                                const PvPathInfo &info)
{
    if (!this->colorMap || nx <= 0 || ny <= 0
        || positions.size() != nx
        || spectral.size() != ny
        || intensities.size() != nx * ny) {
        return;
    }

    this->setWindowTitle(u"PV Diagram"_s);
    updateHeaderUi(info);
    m_stepArcsec = info.stepArcsec;
    if (m_widthArcsecSpin) {
        const QSignalBlocker block(m_widthArcsecSpin);
        m_widthArcsecSpin->setEnabled(m_stepArcsec > 0.0);
        // Decimals from the pixel scale: an ALMA pixel of 0.003 arcsec read
        // "0.00" with two (#520). Enough for one pixel to show, at most six.
        const int decimals = m_stepArcsec > 0.0
                ? std::clamp(int(std::ceil(-std::log10(m_stepArcsec))) + 2, 2, 6)
                : 2;
        m_widthArcsecSpin->setDecimals(decimals);
        m_widthArcsecSpin->setSingleStep(m_stepArcsec > 0.0 ? m_stepArcsec : 0.01);
        m_widthArcsecSpin->setValue(m_stepArcsec > 0.0
                                            ? pvpath::widthArcsec(info.widthPixels, m_stepArcsec) : 0.0);
    }
    this->titleElement->setText(title);
    this->plot->xAxis->setLabel(xLabel);
    this->plot->yAxis->setLabel(yLabel);

    const double xMin = positions.first();
    const double xMax = positions.last();
    const double yMin = *std::min_element(spectral.cbegin(), spectral.cend());
    const double yMax = *std::max_element(spectral.cbegin(), spectral.cend());

    // QCPColorMap spreads row 0 at yMin … row ny-1 at yMax. The rows arrive in
    // CHANNEL order, which runs downwards on a cube with CDELT3 < 0 (most HI
    // cubes): written as they come, the diagram was mirrored against its own
    // axis and emission landed at the wrong velocity.
    const pvpath::UniformRows grid = pvpath::rowsOnUniformSpectralGrid(
            std::vector<double>(intensities.cbegin(), intensities.cend()), nx, ny,
            std::vector<double>(spectral.cbegin(), spectral.cend()));
    const std::vector<double> &rows = grid.values;
    const int plotRows = grid.rows;

    this->colorMap->data()->setSize(nx, plotRows);
    this->colorMap->data()->setRange(QCPRange(xMin, xMax), QCPRange(yMin, yMax));

    double minValue = std::numeric_limits<double>::infinity();
    double maxValue = -std::numeric_limits<double>::infinity();
    for (const double value : rows) {
        if (std::isfinite(value)) {
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
    }
    // applyScale() writes the cells: under Log it lifts what lies below the
    // floor, so the values are kept as they came.
    m_cells = rows;
    m_cellColumns = nx;
    m_cellRows = plotRows;

    if (!std::isfinite(minValue) || !std::isfinite(maxValue) || minValue == maxValue) {
        minValue = 0.0;
        maxValue = minValue + 1.0;
    }

    m_dataMin = minValue;
    m_dataMax = maxValue;
    // The log floor by the maps' rule (LutScale::sampledPositiveFloor): the 1st
    // percentile of the positive values, so the bar starts near the faint end
    // instead of six decades below the peak.
    std::vector<double> positives;
    positives.reserve(rows.size());
    for (const double v : rows)
        if (std::isfinite(v) && v > 0.0)
            positives.push_back(v);
    m_logFloor = LutScale::percentileFloor(std::move(positives));
    // A new cut with nothing positive cannot stay logarithmic.
    if (m_scale == LutScale::Mode::Log && !(m_logFloor > 0.0 && m_logFloor < m_dataMax)) {
        m_scale = LutScale::Mode::Linear;
        emit logScaleUnavailable();
    }
    applyScale();
    if (this->colorScale)
        this->colorScale->axis()->setLabel(info.bunit);
    this->plot->rescaleAxes();
    // The numbered vertices of the path on the offset axis: a dashed line and
    // its number at each, as on the image.
    for (QCPAbstractItem *item : std::as_const(m_vertexItems))
        this->plot->removeItem(item);
    m_vertexItems.clear();
    for (int i = 0; i < info.vertexOffsets.size(); ++i) {
        const double x = info.vertexOffsets[i];
        auto *line = new QCPItemLine(this->plot);
        line->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
        line->end->setTypeY(QCPItemPosition::ptAxisRectRatio);
        line->start->setCoords(x, 0.0);
        line->end->setCoords(x, 1.0);
        // Visible over a bright PV map too: wider, opaque, a dash with gaps.
        QPen markPen(QColor(255, 184, 51), 1.6, Qt::CustomDashLine);
        markPen.setDashPattern({ 5.0, 3.0 });
        line->setPen(markPen);
        auto *label = new QCPItemText(this->plot);
        label->position->setTypeY(QCPItemPosition::ptAxisRectRatio);
        label->position->setCoords(x, 0.0);
        // The first and last vertex sit on the plot's edges: their numbers stay inside.
        const bool last = i == info.vertexOffsets.size() - 1;
        label->setPositionAlignment((i == 0 ? Qt::AlignLeft : last ? Qt::AlignRight : Qt::AlignHCenter)
                                    | Qt::AlignTop);
        label->setText(pvpath::vertexName(std::size_t(i)));
        label->setColor(QColor(255, 184, 51));
        label->setBrush(QBrush(QColor(0, 0, 0, 120)));
        label->setPadding(QMargins(3, 1, 3, 1));
        m_vertexItems << line << label;
    }
    this->plot->replot();
    this->show();
    this->raise();
    this->activateWindow();
}

void PvDiagramWidget::setPathControls(int widthPixels, bool smooth)
{
    const QSignalBlocker b1(m_widthSpin);
    const QSignalBlocker b2(m_smoothCheck);
    m_widthSpin->setValue(widthPixels);
    if (m_widthArcsecSpin && m_stepArcsec > 0.0) {
        const QSignalBlocker b3(m_widthArcsecSpin);
        m_widthArcsecSpin->setValue(pvpath::widthArcsec(widthPixels, m_stepArcsec));
    }
    m_smoothCheck->setChecked(smooth);
}

void PvDiagramWidget::setRangeControls(const SpectralRangeInput &range, const QString &valueUnit)
{
    const QSignalBlocker b1(m_rangeMode);
    const QSignalBlocker b2(m_rangeFrom);
    const QSignalBlocker b3(m_rangeTo);
    m_rangeMode->setItemText(1, valueUnit.isEmpty() ? u"Axis values"_s
                                                    : u"Axis (%1)"_s.arg(valueUnit));
    m_rangeMode->setCurrentIndex(range.byValue ? 1 : 0);
    const auto show = [](QLineEdit *edit, double v) {
        edit->setText(std::isfinite(v) ? QString::number(v, 'g', 10) : QString());
    };
    m_lastRange = range;
    show(m_rangeFrom, range.from);
    show(m_rangeTo, range.to);
}

void PvDiagramWidget::rangeEdited()
{
    const auto read = [](const QLineEdit *edit) {
        bool ok = false;
        const double v = QLocale::c().toDouble(edit->text().trimmed(), &ok);
        return ok ? v : std::numeric_limits<double>::quiet_NaN();
    };
    const bool byValue = m_rangeMode->currentIndex() == 1;
    const double from = read(m_rangeFrom), to = read(m_rangeTo);
    // editingFinished also fires on a plain loss of focus: only a change recomputes.
    const auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
    if (byValue == m_lastRange.byValue && same(from, m_lastRange.from) && same(to, m_lastRange.to))
        return;
    m_lastRange = { byValue, from, to };
    emit spectralRangeChanged(byValue, from, to);
}

void PvDiagramWidget::widthArcsecEdited()
{
    if (!(m_stepArcsec > 0.0))
        return;
    const int px = pvpath::widthPixelsForArcsec(m_widthArcsecSpin->value(), m_stepArcsec);
    if (px == m_widthSpin->value()) {
        // The same whole width: show it as such.
        const QSignalBlocker block(m_widthArcsecSpin);
        m_widthArcsecSpin->setValue(pvpath::widthArcsec(px, m_stepArcsec));
        return;
    }
    m_widthSpin->setValue(px);   // emits widthChanged; the result re-syncs the arcsec box
    // At the spin's limit nothing changed: show the width in force.
    const QSignalBlocker block(m_widthArcsecSpin);
    m_widthArcsecSpin->setValue(pvpath::widthArcsec(m_widthSpin->value(), m_stepArcsec));
}

bool PvDiagramWidget::setScale(LutScale::Mode mode)
{
    if (mode == m_scale)
        return true;
    if (mode == LutScale::Mode::Log && !(m_logFloor > 0.0 && m_logFloor < m_dataMax)) {
        emit logScaleUnavailable();
        return false;
    }
    m_scale = mode;
    applyScale();
    if (this->plot)
        this->plot->replot();
    return true;
}

void PvDiagramWidget::applyScale()
{
    if (!this->colorMap)
        return;
    const bool log = (m_scale == LutScale::Mode::Log);
    // Under Log, what lies below the floor (noise, negatives, zero) is drawn at
    // the floor: the bottom colour, as on a map pane. QCustomPlot takes the log
    // of every cell, and the log of a non-positive value has no colour.
    for (int y = 0; y < m_cellRows; ++y) {
        for (int x = 0; x < m_cellColumns; ++x) {
            const double v = m_cells[std::size_t(y) * m_cellColumns + x];
            this->colorMap->data()->setCell(
                    x, y, (log && std::isfinite(v)) ? std::max(v, m_logFloor) : v);
        }
    }
    QCPColorGradient jet(QCPColorGradient::gpJet);   // color() is not const
    QCPColorGradient gradient = jet;
    if (LutScale::isResampled(m_scale)) {
        // The maps' stretch (LutScale::build): a value at normalised t takes the
        // colour the map holds at t^exponent. The bar keeps its linear axis, as
        // the maps' does, and shows the stretched colours along it.
        const double exp = LutScale::exponent(m_scale, 2.0);
        constexpr int kStops = 64;
        gradient.clearColorStops();
        for (int i = 0; i <= kStops; ++i) {
            const double t = static_cast<double>(i) / kStops;
            gradient.setColorStopAt(t, QColor::fromRgb(jet.color(std::pow(t, exp),
                                                                 QCPRange(0.0, 1.0))));
        }
    }
    if (this->colorScale) {
        // The bar owns the map's scale type once they are linked.
        this->colorScale->setDataScaleType(log ? QCPAxis::stLogarithmic : QCPAxis::stLinear);
        if (log)
            this->colorScale->axis()->setTicker(QSharedPointer<QCPAxisTickerLog>::create());
        else
            this->colorScale->axis()->setTicker(QSharedPointer<QCPAxisTicker>::create());
    } else {
        this->colorMap->setDataScaleType(log ? QCPAxis::stLogarithmic : QCPAxis::stLinear);
    }
    this->colorMap->setGradient(gradient);
    this->colorMap->setDataRange(log ? QCPRange(m_logFloor, m_dataMax)
                                     : QCPRange(m_dataMin, m_dataMax));
}
