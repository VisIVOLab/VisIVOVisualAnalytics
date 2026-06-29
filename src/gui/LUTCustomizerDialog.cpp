#include "LUTCustomizerDialog.h"
#include "ui_LUTCustomizerDialog.h"

#include "ColorMaps.h"
#include "theme/SegmentedToggle.h"

#include <vtkDataSetAttributes.h>
#include <vtkDoubleArray.h>
#include <vtkExtractHistogram.h>
#include <vtkImageData.h>
#include <vtkIntArray.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkTable.h>

#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QDebug>
#include <QGridLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSize>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

namespace {
constexpr vtkIdType histogramTargetSamples = 250000;

// Gradient preview icon for combobox — same pattern used in vtkWindowImage and vtkWindowVbt.
static QIcon makeColormapIcon(const std::string &name, int iconW = 80, int iconH = 14)
{
    vtkNew<vtkLookupTable> lut;
    lut->SetNumberOfTableValues(256);
    lut->SetTableRange(0.0, 1.0);
    ColorMaps::SetColorMap(lut.Get(), name);
    lut->Build();
    QImage img(iconW, iconH, QImage::Format_RGB32);
    for (int x = 0; x < iconW; ++x) {
        // Sample via GetColor (maps through the table range) rather than
        // GetTableValue(idx): some colormaps (e.g. VolRenTwoLev) define only a
        // few entries, and a fixed 0..255 index would clamp to the last (grey).
        const double t = static_cast<double>(x) / std::max(1, iconW - 1);
        double rgb[3];
        lut->GetColor(t, rgb);
        const QRgb c = qRgb(static_cast<int>(rgb[0] * 255.0),
                            static_cast<int>(rgb[1] * 255.0),
                            static_cast<int>(rgb[2] * 255.0));
        for (int y = 0; y < iconH; ++y)
            img.setPixel(x, y, c);
    }
    return QIcon(QPixmap::fromImage(img));
}
constexpr int histogramBinCount = 512;
constexpr double logAxisEpsilon = 1e-12;

double sampledPositiveFloor(vtkImageData *dataset)
{
    if (!dataset) {
        return 0.0;
    }
    auto *scalars =
            dataset->GetPointData() ? dataset->GetPointData()->GetScalars() : nullptr;
    const vtkIdType nels = dataset->GetNumberOfPoints();
    if (!scalars || nels <= 0) {
        return 0.0;
    }

    const vtkIdType stride =
            std::max<vtkIdType>(1, static_cast<vtkIdType>(std::ceil(
                                           static_cast<double>(nels) / histogramTargetSamples)));
    std::vector<double> positives;
    positives.reserve(static_cast<std::size_t>(std::min<vtkIdType>(nels / stride, 32768)));
    for (vtkIdType i = 0; i < nels; i += stride) {
        const double value = scalars->GetComponent(i, 0);
        if (std::isfinite(value) && value > 0.0) {
            positives.push_back(value);
        }
    }
    if (positives.empty()) {
        return 0.0;
    }
    std::sort(positives.begin(), positives.end());
    const std::size_t index =
            std::min<std::size_t>(positives.size() - 1,
                                  static_cast<std::size_t>(std::floor((positives.size() - 1) * 0.01)));
    return std::max(logAxisEpsilon, positives[index]);
}
}

LUTCustomizerDialog::LUTCustomizerDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::LUTCustomizerDialog), selectedLine(nullptr)
{
    ui->setupUi(this);
    this->setAttribute(Qt::WA_DeleteOnClose);

    this->setupPlot();

    // Setup line edits
    ui->lineMin->setValidator(new QDoubleValidator(ui->lineMin));
    ui->lineMax->setValidator(new QDoubleValidator(ui->lineMax));
    QObject::connect(ui->lineMin, &QLineEdit::textEdited, this,
                     &LUTCustomizerDialog::updateReferenceLines);
    QObject::connect(ui->lineMax, &QLineEdit::textEdited, this,
                     &LUTCustomizerDialog::updateReferenceLines);

    // Setup reset buttons
    ui->btnResetMin->setIcon(QIcon(u":/icons/RESET.png"_s));
    ui->btnResetMax->setIcon(QIcon(u":/icons/RESET.png"_s));
    ui->btnResetMin->setToolTip(u"Reset min to data minimum"_s);
    ui->btnResetMax->setToolTip(u"Reset max to data maximum"_s);
    QObject::connect(ui->btnResetMin, &QToolButton::clicked, this, &LUTCustomizerDialog::resetMin);
    QObject::connect(ui->btnResetMax, &QToolButton::clicked, this, &LUTCustomizerDialog::resetMax);

    // Color Maps Combobox — gradient preview icons (same pattern as other viewers)
    ui->comboLut->setIconSize(QSize(80, 14));
    auto cmaps = ColorMaps::GetColorMapNames();
    std::for_each(cmaps.cbegin(), cmaps.cend(), [this](const std::string &name) {
        ui->comboLut->addItem(makeColormapIcon(name), QString::fromStdString(name));
    });
    ui->comboLut->setCurrentText(QString::fromStdString(ColorMaps::DefaultColorMap));

    // Colour-scale stretch — segmented Linear / Log / Sqrt / Square / Power
    // control (the SegmentedToggle exposes a QComboBox-flavoured API). The
    // mode list + tool-tips come from the shared LutScale helper so every
    // viewer stays consistent.
    {
        const auto labels = LutScale::modeLabels();
        const auto tips = LutScale::modeToolTips();
        for (std::size_t i = 0; i < labels.size(); ++i) {
            // fromUtf8: the tool-tips contain non-ASCII (√t, t², γ, em-dash).
            ui->comboScale->addItem(QString::fromUtf8(labels[i]),
                                    QString::fromUtf8(tips[i]));
        }
    }
    QObject::connect(ui->comboScale, &SegmentedToggle::currentIndexChanged, this,
                     &LUTCustomizerDialog::changeScaleMode);

    // Power exponent (γ) — only relevant when the Power mode is selected.
    this->gammaSpin = new QDoubleSpinBox(this);
    this->gammaSpin->setRange(0.1, 10.0);
    this->gammaSpin->setSingleStep(0.1);
    this->gammaSpin->setDecimals(2);
    this->gammaSpin->setValue(this->m_gamma);
    this->gammaSpin->setToolTip(u"Exponent γ for the Power stretch (t^γ)."_s);
    this->gammaLabel = new QLabel(u"Power γ"_s, this);
    if (auto *grid = qobject_cast<QGridLayout *>(ui->groupLut->layout())) {
        grid->addWidget(this->gammaLabel, 4, 0);
        grid->addWidget(this->gammaSpin, 4, 1, 1, 2);
    }
    this->gammaLabel->setVisible(false);
    this->gammaSpin->setVisible(false);
    QObject::connect(this->gammaSpin, &QDoubleSpinBox::valueChanged, this,
                     [this](double v) { this->m_gamma = v; });

    // Apply button
    QObject::connect(ui->buttonBox, &QDialogButtonBox::clicked, this,
                     &LUTCustomizerDialog::buttonClicked);

    // Hint label inserted between the plot and the Lookup Table group
    auto *hintLbl = new QLabel(
            u"Drag the \u25cf cyan (Min) or \u25cf orange (Max) line \u2014 or edit values below."_s,
            this);
    hintLbl->setStyleSheet(u"QLabel { color: #607080; font-size: 9px; padding: 0 4px; }"_s);
    hintLbl->setAlignment(Qt::AlignCenter);
    if (auto *vl = qobject_cast<QVBoxLayout *>(this->layout()))
        vl->insertWidget(1, hintLbl);
}

LUTCustomizerDialog::~LUTCustomizerDialog()
{
    delete ui;
}

void LUTCustomizerDialog::setupPlot()
{
    // ── Dark background matching the app theme ────────────────────────────────
    ui->plot->setBackground(QBrush(QColor(12, 22, 34)));
    ui->plot->axisRect()->setBackground(QBrush(QColor(8, 16, 26)));
    const QPen axisPen(QColor(80, 110, 140));
    const QPen subTickPen(QColor(55, 75, 100));
    const QColor tickLabelColor(155, 180, 210);
    for (auto *axis : { ui->plot->xAxis, ui->plot->yAxis }) {
        axis->setBasePen(axisPen);
        axis->setTickPen(axisPen);
        axis->setSubTickPen(subTickPen);
        axis->setTickLabelColor(tickLabelColor);
        axis->setLabelColor(tickLabelColor);
    }
    // Astrophysical cubes are dominated by background / near-zero voxels.
    // On a linear Y axis the background bin can be ~10⁵× larger than any
    // signal bin, flattening everything else to a single pixel of height —
    // the user effectively sees one spike and nothing else. Logarithmic Y
    // surfaces both the background peak and the long emission tail, which
    // is what the LUT customiser really wants to display. The Y ticks stay
    // shown (raw counts) so the user can read the dynamic range.
    ui->plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
    ui->plot->yAxis->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
    ui->plot->yAxis->setTickLabels(true);
    ui->plot->yAxis->setNumberFormat(u"eb"_s);   // "10^N" exponent format
    ui->plot->yAxis->setNumberPrecision(0);

    ui->plot->setInteractions(QCP::iSelectItems | QCP::iRangeDrag);
    ui->plot->axisRect()->setRangeDrag(Qt::Horizontal);

    // ── Histogram graph — impulse style (vertical bars from y=0). lsImpulse
    // ── reads correctly on both linear and log Y, unlike lsStepCenter which
    // ── shows step transitions even where adjacent bins are zero.
    ui->plot->addGraph();
    ui->plot->graph()->setLineStyle(QCPGraph::lsImpulse);
    ui->plot->graph()->setPen(QPen(QColor(50, 150, 230, 220), 1));

    // ── Min marker — cyan, width 2 (QCPItemStraightLine extends to plot edges) ─
    this->refLineMin = new QCPItemStraightLine(ui->plot);
    QPen minPen(QColor(0, 190, 240));
    minPen.setWidth(2);
    this->refLineMin->setPen(minPen);
    QPen minSelPen(QColor(80, 225, 255));
    minSelPen.setWidth(3);
    this->refLineMin->setSelectedPen(minSelPen);
    this->refLineMin->point1->setCoords(0, 0);
    this->refLineMin->point2->setCoords(0, 1);

    // ── Max marker — orange, width 2 ─────────────────────────────────────────
    this->refLineMax = new QCPItemStraightLine(ui->plot);
    QPen maxPen(QColor(255, 140, 0));
    maxPen.setWidth(2);
    this->refLineMax->setPen(maxPen);
    QPen maxSelPen(QColor(255, 205, 60));
    maxSelPen.setWidth(3);
    this->refLineMax->setSelectedPen(maxSelPen);
    this->refLineMax->point1->setCoords(0, 0);
    this->refLineMax->point2->setCoords(0, 1);

    QObject::connect(ui->plot, &QCustomPlot::itemClick, this,
                     &LUTCustomizerDialog::selectReferenceLine);
    QObject::connect(ui->plot, &QCustomPlot::mouseMove, this,
                     &LUTCustomizerDialog::moveReferenceLine);
    QObject::connect(ui->plot, &QCustomPlot::mouseRelease, this,
                     &LUTCustomizerDialog::deselectReferenceLine);
}

void LUTCustomizerDialog::plotHistogram()
{
    QElapsedTimer timer;
    timer.start();
    qDebug().noquote() << QStringLiteral("[lut] histogram update started");
    if (!this->dataset) {
        return;
    }

    const vtkIdType nels = this->dataset->GetNumberOfPoints();
    if (nels <= 0 || !std::isfinite(this->datasetRange[0]) || !std::isfinite(this->datasetRange[1])
        || this->datasetRange[0] >= this->datasetRange[1]) {
        return;
    }

    auto *scalars =
            this->dataset->GetPointData() ? this->dataset->GetPointData()->GetScalars() : nullptr;
    if (!scalars) {
        return;
    }

    const vtkIdType stride =
            std::max<vtkIdType>(1, static_cast<vtkIdType>(std::ceil(
                                           static_cast<double>(nels) / histogramTargetSamples)));
    const double minValue = this->datasetRange[0];
    const double maxValue = this->datasetRange[1];
    const double binWidth = (maxValue - minValue) / static_cast<double>(histogramBinCount);
    if (!std::isfinite(binWidth) || binWidth <= 0.0) {
        return;
    }

    std::vector<double> bins(histogramBinCount, 0.0);
    for (vtkIdType i = 0; i < nels; i += stride) {
        const double value = scalars->GetComponent(i, 0);
        if (!std::isfinite(value)) {
            continue;
        }
        int bin = static_cast<int>(std::floor((value - minValue) / binWidth));
        bin = std::clamp(bin, 0, histogramBinCount - 1);
        bins[static_cast<std::size_t>(bin)] += 1.0;
    }

    QVector<double> x(histogramBinCount), y(histogramBinCount);
    for (int i = 0; i < histogramBinCount; ++i) {
        x[i] = minValue + (i + 0.5) * binWidth;
        y[i] = bins[static_cast<std::size_t>(i)];
    }
    ui->plot->graph()->setData(x, y, true);
    // Graph pen/brush are set once in setupPlot() — don't override here.
    //
    // Y range: rescale on the actual bin counts. On a logarithmic Y axis the
    // lower bound must be > 0; QCustomPlot's rescaleAxes() already handles
    // this for log scale by ignoring zero/negative values, but we set an
    // explicit floor of 1 (count) to make sure the first decade is visible
    // regardless of the bin distribution.
    ui->plot->graph()->rescaleValueAxis(false, true);   // y = counts
    {
        QCPRange yr = ui->plot->yAxis->range();
        if (ui->plot->yAxis->scaleType() == QCPAxis::stLogarithmic) {
            yr.lower = std::max(1.0, yr.lower);
            yr.upper = std::max(yr.upper, yr.lower * 10.0);
        } else {
            yr.lower = 0.0;
        }
        ui->plot->yAxis->setRange(yr);
    }
    // X range is set authoritatively by changeAxisScaling() below — it knows
    // whether log or linear is active and uses the positive-floor estimate to
    // avoid log(0)/log(negative) issues when the dataset spans zero.

    // QCPItemStraightLine is defined by two points; any two points with the same x
    // give an infinite vertical line — no Y-range bookkeeping needed.
    const double *lutRange = this->lut->GetTableRange();
    this->refLineMin->point1->setCoords(lutRange[0], 0);
    this->refLineMin->point2->setCoords(lutRange[0], 1);
    this->refLineMax->point1->setCoords(lutRange[1], 0);
    this->refLineMax->point2->setCoords(lutRange[1], 1);

    this->changeAxisScaling(this->m_scaleMode == LutScale::Mode::Log);
    qDebug().noquote()
            << QStringLiteral("[lut] histogram update completed ms=%1").arg(timer.elapsed());
}

void LUTCustomizerDialog::changeScaleMode(int index)
{
    this->m_scaleMode = static_cast<LutScale::Mode>(std::clamp(index, 0, 4));
    const bool power = (this->m_scaleMode == LutScale::Mode::Power);
    if (this->gammaLabel) {
        this->gammaLabel->setVisible(power);
    }
    if (this->gammaSpin) {
        this->gammaSpin->setVisible(power);
    }
    // The histogram x-axis only switches to logarithmic for the Log mode; the
    // Sqrt/Square/Power stretches keep a linear value axis (the non-linearity
    // lives in the colour mapping, not the data positions).
    this->changeAxisScaling(this->m_scaleMode == LutScale::Mode::Log);
}

void LUTCustomizerDialog::changeAxisScaling(bool logarithmic)
{
    const double positiveFloor = sampledPositiveFloor(this->dataset);
    const bool safeLogarithmic = logarithmic && positiveFloor > 0.0;
    if (safeLogarithmic) {
        ui->plot->xAxis->setScaleType(QCPAxis::stLogarithmic);
        ui->plot->xAxis->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
        const double upper = std::max(this->datasetRange[1], positiveFloor * 10.0);
        ui->plot->xAxis->setRange(positiveFloor, upper);
    } else {
        ui->plot->xAxis->setScaleType(QCPAxis::stLinear);
        ui->plot->xAxis->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
        // Linear range: include the whole dataset value range with a small
        // padding so the cyan/orange markers are not clipped at the borders.
        if (std::isfinite(this->datasetRange[0]) && std::isfinite(this->datasetRange[1])
            && this->datasetRange[0] < this->datasetRange[1]) {
            const double pad = 0.02 * (this->datasetRange[1] - this->datasetRange[0]);
            ui->plot->xAxis->setRange(this->datasetRange[0] - pad,
                                      this->datasetRange[1] + pad);
        }
    }

    ui->plot->replot();
}

void LUTCustomizerDialog::init(vtkImageData *dataset, vtkLookupTable *lut)
{
    // Back-compat entry point: infer Linear/Log from the table's native scale
    // (callers that track Sqrt/Square/Power use the mode-aware overload below).
    const LutScale::Mode mode =
            (lut && lut->UsingLogScale()) ? LutScale::Mode::Log : LutScale::Mode::Linear;
    this->init(dataset, lut, mode, this->m_gamma);
}

void LUTCustomizerDialog::init(vtkImageData *dataset, vtkLookupTable *lut,
                               LutScale::Mode mode, double gamma)
{
    if (!dataset || !lut) {
        return;
    }
    this->dataset = dataset;
    this->dataset->GetScalarRange(this->datasetRange);

    this->lut = lut;
    this->m_scaleMode = mode;
    this->m_gamma = gamma;
    const double *lutRange = this->lut->GetTableRange();
    ui->lineMin->setText(QString::number(lutRange[0]));
    ui->lineMax->setText(QString::number(lutRange[1]));
    ui->comboLut->setCurrentText(QString::fromStdString(this->lut->GetObjectName()));
    if (this->gammaSpin) {
        const QSignalBlocker blocker(this->gammaSpin);
        this->gammaSpin->setValue(gamma);
    }
    ui->comboScale->setCurrentIndex(static_cast<int>(mode));
    // Ensure gamma-row visibility + plot axis match even if the combo did not
    // emit (e.g. when the index was already at `mode`).
    this->changeScaleMode(static_cast<int>(mode));

    this->plotHistogram();
}

void LUTCustomizerDialog::resetMin()
{
    ui->lineMin->setText(QString::number(this->datasetRange[0]));
    this->refLineMin->point1->setCoords(this->datasetRange[0], 0);
    this->refLineMin->point2->setCoords(this->datasetRange[0], 1);
    ui->plot->replot();
}

void LUTCustomizerDialog::resetMax()
{
    ui->lineMax->setText(QString::number(this->datasetRange[1]));
    this->refLineMax->point1->setCoords(this->datasetRange[1], 0);
    this->refLineMax->point2->setCoords(this->datasetRange[1], 1);
    ui->plot->replot();
}

void LUTCustomizerDialog::selectReferenceLine(QCPAbstractItem *item)
{
    this->selectedLine = qobject_cast<QCPItemStraightLine *>(item);
    ui->plot->setInteraction(QCP::iRangeDrag, false);
}

void LUTCustomizerDialog::moveReferenceLine(QMouseEvent *event)
{
    if (!this->selectedLine || event->buttons().testFlag(Qt::NoButton)) {
        return;
    }

    double x = ui->plot->xAxis->pixelToCoord(event->pos().x());

    // Enforce Min < Max: prevent the two markers from crossing
    if (this->selectedLine == this->refLineMin) {
        const double maxX = this->refLineMax->point1->coords().x();
        x = std::min(x, maxX - 1e-10);
        ui->lineMin->setText(QString::number(x));
    } else {
        const double minX = this->refLineMin->point1->coords().x();
        x = std::max(x, minX + 1e-10);
        ui->lineMax->setText(QString::number(x));
    }

    this->selectedLine->point1->setCoords(x, 0);
    this->selectedLine->point2->setCoords(x, 1);
    ui->plot->replot();
}

void LUTCustomizerDialog::deselectReferenceLine()
{
    this->selectedLine = nullptr;
    ui->plot->setInteraction(QCP::iRangeDrag, true);
    ui->plot->deselectAll();
    ui->plot->replot();
}

void LUTCustomizerDialog::updateReferenceLines()
{
    const double minX = ui->lineMin->text().toDouble();
    const double maxX = ui->lineMax->text().toDouble();
    this->refLineMin->point1->setCoords(minX, 0);
    this->refLineMin->point2->setCoords(minX, 1);
    this->refLineMax->point1->setCoords(maxX, 0);
    this->refLineMax->point2->setCoords(maxX, 1);
    ui->plot->replot();
}

void LUTCustomizerDialog::updateLut()
{
    if (!this->lut) {
        return;
    }
    double minValue = ui->lineMin->text().toDouble();
    double maxValue = ui->lineMax->text().toDouble();
    if (!std::isfinite(minValue) || !std::isfinite(maxValue) || minValue >= maxValue) {
        return;
    }
    this->m_scaleMode = static_cast<LutScale::Mode>(std::clamp(ui->comboScale->currentIndex(), 0, 4));
    this->m_gamma = this->gammaSpin ? this->gammaSpin->value() : this->m_gamma;

    if (this->m_scaleMode == LutScale::Mode::Log) {
        // Dataset-aware positive floor (more accurate than a generic epsilon)
        // so the log range starts at the smallest real positive sample.
        const double positiveFloor = sampledPositiveFloor(this->dataset);
        if (positiveFloor <= 0.0) {
            return;
        }
        maxValue = std::max(maxValue, positiveFloor);
        minValue = std::max(minValue, positiveFloor);
    }

    // build() forces a fresh linear colormap fill before stretching, so repeated
    // Apply clicks (or a mode change without a colormap change) never compound.
    LutScale::build(this->lut, ui->comboLut->currentText().toStdString(), this->m_scaleMode,
                    minValue, maxValue, this->m_gamma);
    emit this->lutUpdated();
}

void LUTCustomizerDialog::buttonClicked(QAbstractButton *btn)
{
    if (ui->buttonBox->buttonRole(btn) == QDialogButtonBox::ApplyRole) {
        this->updateLut();
    }
}
