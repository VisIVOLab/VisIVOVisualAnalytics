#include "ProfileWidget.h"
#include "ui_ProfileWidget.h"

#include "vtkInteractorStyleProfile.h"

#include <vtkImageData.h>

#include <QColor>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPushButton>

using namespace Qt::StringLiterals;

/// Build the save-PNG footer bar using the same pattern as ToolResultDialog:
/// a QDialogButtonBox with ActionRole buttons lets the platform style size the
/// buttons correctly without any manual width calculation.
///
/// The footer is appended to @p widget's existing QVBoxLayout (installed by
/// setupUi), and the plot rows are given stretch=1 so the footer stays compact.
static void buildSaveFooter(QWidget *widget,
                             QPushButton *&btn1Out, QPushButton *&btn2Out)
{
    // ── Footer widget — same structure as ToolResultDialog's footer ───────────
    auto *footer = new QWidget(widget);
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 10, 12, 10);

    // QDialogButtonBox delegates sizing to the native platform style.
    // On macOS this produces correctly-padded Aqua-style buttons — no
    // manual setFixedWidth / fontMetrics needed.
    auto *buttonBox = new QDialogButtonBox(footer);

    btn1Out = new QPushButton(u"Save plot 1 as PNG\u2026"_s);
    btn2Out = new QPushButton(u"Save plot 2 as PNG\u2026"_s);
    buttonBox->addButton(btn1Out, QDialogButtonBox::ActionRole);
    buttonBox->addButton(btn2Out, QDialogButtonBox::ActionRole);

    footerLayout->addWidget(buttonBox);

    // ── Attach to the outer VBoxLayout installed by setupUi ──────────────────
    // Layout indices: [0]=checkLive, [1]=plot1, [2]=plot2  (from UI file)
    auto *vLayout = qobject_cast<QVBoxLayout *>(widget->layout());
    vLayout->setContentsMargins(0, 0, 0, 0);  // remove platform-default 9 px margins
    vLayout->setStretch(1, 1);  // plot1 — expand
    vLayout->setStretch(2, 1);  // plot2 — expand (0-height when hidden)
    vLayout->addWidget(footer); // footer appended at stretch=0 (default)
}

ProfileWidget::ProfileWidget(vtkInteractorStyleProfile *style, vtkImageData *dataset,
                             const std::string &filepath, QWidget *parent)
    : QWidget(parent),
      ui(new Ui::ProfileWidget),
      dataset(dataset),
      astro(std::make_unique<AstroUtils>(filepath)),
      interactor(style)
{
    ui->setupUi(this);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowFlag(Qt::Window);

    buildSaveFooter(this, m_btnSavePlot1, m_btnSavePlot2);
    QObject::connect(m_btnSavePlot1, &QPushButton::clicked, this, &ProfileWidget::savePlot1Png);
    QObject::connect(m_btnSavePlot2, &QPushButton::clicked, this, &ProfileWidget::savePlot2Png);

    // Get info from dataset
    this->ScalarPointer = static_cast<float *>(this->dataset->GetScalarPointer());
    this->dataset->GetIncrements(this->Increments);
    this->dataset->GetDimensions(this->Dimensions);

    // Setup Interactor
    this->interactor->SetLengths(this->Dimensions[0] - 1, this->Dimensions[1] - 1);
    QObject::connect(ui->checkLive, &QCheckBox::toggled, this, &ProfileWidget::setLiveMode);
}

ProfileWidget::ProfileWidget(QWidget *parent)
    : QWidget(parent),
      ui(new Ui::ProfileWidget),
      dataset(nullptr),
      astro(nullptr),
      ScalarPointer(nullptr),
      Dimensions{ 0, 0, 0 },
      Increments{ 0, 0, 0 },
      interactor(nullptr),
      refLineX(nullptr),
      refLineY(nullptr)
{
    ui->setupUi(this);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowFlag(Qt::Window);

    buildSaveFooter(this, m_btnSavePlot1, m_btnSavePlot2);
    QObject::connect(m_btnSavePlot1, &QPushButton::clicked, this, &ProfileWidget::savePlot1Png);
    QObject::connect(m_btnSavePlot2, &QPushButton::clicked, this, &ProfileWidget::savePlot2Png);
}

ProfileWidget::~ProfileWidget()
{
    delete ui;
}

void ProfileWidget::setUsageMode(UsageMode mode, const QString &windowTitle)
{
    this->usageMode = mode;
    this->applyUsageModeUi(windowTitle);
}

void ProfileWidget::applyUsageModeUi(const QString &windowTitle)
{
    const bool liveMode = this->usageMode == UsageMode::ProbeLive;
    ui->checkLive->setVisible(liveMode);
    ui->checkLive->setEnabled(liveMode);
    if (!liveMode) {
        ui->checkLive->setChecked(false);
        if (this->interactor) {
            this->interactor->SetLiveMode(false);
        }
    }

    if (!windowTitle.isEmpty()) {
        this->setWindowTitle(windowTitle);
    } else {
        this->setWindowTitle(liveMode ? u"Profile"_s : u"Region Profile"_s);
    }
}

void ProfileWidget::setupImagePlots()
{
    this->applyUsageModeUi();
    if (!this->interactor || !this->astro) {
        return;
    }
    this->interactor->SetCallback(
            [this](double x, double y, bool live) { this->plotProfile(x, y, live); });

    // Setup plots
    const std::string unit = this->astro->getPhysicalUnit();
    const QString yLabel = unit.empty() ? u"Value"_s : QString::fromStdString(unit);

    ui->plot1->addGraph();
    ui->plot1->setInteractions(QCP::iRangeDrag);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot1->plotLayout()->insertRow(0);
    ui->plot1->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot1, u"X Profile"_s));
    ui->plot1->xAxis->setLabel(u"X Coordinate"_s);
    ui->plot1->yAxis->setLabel(yLabel);
    this->refLineX = new QCPItemLine(ui->plot1);
    this->refLineX->setPen({ Qt::red });

    ui->plot2->addGraph();
    ui->plot2->setInteractions(QCP::iRangeDrag);
    ui->plot2->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot2->plotLayout()->insertRow(0);
    ui->plot2->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot2, u"Y Profile"_s));
    this->refLineY = new QCPItemLine(ui->plot2);
    this->refLineY->setPen({ Qt::red });
    ui->plot2->xAxis->setLabel(u"Y Coordinate"_s);
    ui->plot2->yAxis->setLabel(yLabel);
}

void ProfileWidget::setupImagePlots(const QString &xLabel, const QString &yLabel)
{
    this->applyUsageModeUi();
    ui->plot1->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->plot2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->plot1->clearGraphs();
    ui->plot2->clearGraphs();
    ui->plot1->addGraph();
    ui->plot2->addGraph();
    ui->plot1->setInteractions(QCP::iRangeDrag);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot2->setInteractions(QCP::iRangeDrag);
    ui->plot2->axisRect()->setRangeDrag(Qt::Horizontal);
    if (!ui->plot1->plotLayout()->element(0, 0)) {
        ui->plot1->plotLayout()->insertRow(0);
        ui->plot1->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot1, u"X Profile"_s));
    }
    if (!ui->plot2->plotLayout()->element(0, 0)) {
        ui->plot2->plotLayout()->insertRow(0);
        ui->plot2->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot2, u"Y Profile"_s));
    }
    ui->plot1->xAxis->setLabel(u"X Coordinate"_s);
    ui->plot1->yAxis->setLabel(yLabel);
    ui->plot2->xAxis->setLabel(u"Y Coordinate"_s);
    ui->plot2->yAxis->setLabel(yLabel);
    if (!this->refLineX) {
        this->refLineX = new QCPItemLine(ui->plot1);
        this->refLineX->setPen({ Qt::red });
    }
    if (!this->refLineY) {
        this->refLineY = new QCPItemLine(ui->plot2);
        this->refLineY->setPen({ Qt::red });
    }
    Q_UNUSED(xLabel);
}

void ProfileWidget::setupSpectrumPlot()
{
    this->applyUsageModeUi();
    if (!this->astro || !this->interactor) {
        return;
    }
    ui->plot2->hide();

    this->interactor->SetCallback(
            [this](double x, double y, bool live) { this->plotSpectrum(x, y, live); });

    const std::string unit = this->astro->getPhysicalUnit();
    const QString yLabel = unit.empty() ? u"Value"_s : QString::fromStdString(unit);

    const std::string spectralUnit = this->astro->getAxisUnit(2);
    const QString xLabel = spectralUnit.empty() ? u"Value"_s : QString::fromStdString(spectralUnit);

    ui->plot1->addGraph();
    ui->plot1->setInteractions(QCP::iRangeDrag);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot1->plotLayout()->insertRow(0);
    ui->plot1->plotLayout()->addElement(0, 0, new QCPTextElement(ui->plot1, u"Z Profile"_s));
    ui->plot1->xAxis->setLabel(xLabel);
    ui->plot1->yAxis->setLabel(yLabel);
}

void ProfileWidget::setupSpectrumPlot(const QString &xLabel, const QString &yLabel)
{
    this->applyUsageModeUi();
    ui->plot2->hide();
    if (m_btnSavePlot2) m_btnSavePlot2->hide();
    if (m_btnSavePlot1)
        m_btnSavePlot1->setText(u"Save spectrum as PNG\u2026"_s);
    ui->plot1->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->plot1->clearGraphs();
    ui->plot1->addGraph();
    ui->plot1->legend->setVisible(false);
    ui->plot1->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    ui->plot1->axisRect()->setRangeDrag(Qt::Horizontal);
    ui->plot1->plotLayout()->insertRow(0);
    auto *titleEl = new QCPTextElement(ui->plot1, u"Z Profile"_s);
    ui->plot1->plotLayout()->addElement(0, 0, titleEl);
    ui->plot1->xAxis->setLabel(xLabel);
    ui->plot1->yAxis->setLabel(yLabel);

    // Apply default dark theme (caller can override with applyPlotTheme).
    applyPlotTheme(true);
}

void ProfileWidget::setLiveMode(bool live)
{
    if (this->interactor) {
        this->interactor->SetLiveMode(live);
    }
}

void ProfileWidget::plotProfile(double x, double y, bool live)
{
    if (this->usageMode == UsageMode::ProbeLive) {
        ui->checkLive->setChecked(live);
    }
    const long pixX = std::lround(x);
    const long pixY = std::lround(y);

    // X Profile
    QVector<double> keyX(this->Dimensions[0]), valuesX(this->Dimensions[0]);
    for (vtkIdType i = 0; i < this->Dimensions[0]; ++i) {
        const vtkIdType idx = i * this->Increments[0] + pixY * this->Increments[1];
        keyX[i] = i;
        valuesX[i] = this->ScalarPointer[idx];
    }
    ui->plot1->graph()->setData(keyX, valuesX);
    ui->plot1->rescaleAxes();
    ui->plot1->xAxis->setRange(-1., this->Dimensions[0] + 1.);
    this->refLineX->start->setCoords(pixX, ui->plot1->yAxis->range().lower);
    this->refLineX->end->setCoords(pixX, ui->plot1->yAxis->range().upper);
    ui->plot1->replot();

    // Y Profile
    QVector<double> keyY(this->Dimensions[1]), valuesY(this->Dimensions[1]);
    for (vtkIdType j = 0; j < this->Dimensions[1]; ++j) {
        const vtkIdType idx = pixX * this->Increments[0] + j * this->Increments[1];
        keyY[j] = j;
        valuesY[j] = this->ScalarPointer[idx];
    }
    ui->plot2->graph()->setData(keyY, valuesY);
    ui->plot2->rescaleAxes();
    ui->plot2->xAxis->setRange(-1., this->Dimensions[1] + 1.);
    this->refLineY->start->setCoords(pixY, ui->plot2->yAxis->range().lower);
    this->refLineY->end->setCoords(pixY, ui->plot2->yAxis->range().upper);
    ui->plot2->replot();

    this->show();
    this->raise();
}

void ProfileWidget::plotSpectrum(double x, double y, bool live)
{
    if (this->usageMode == UsageMode::ProbeLive) {
        ui->checkLive->setChecked(live);
    }
    const long pixX = std::lround(x);
    const long pixY = std::lround(y);

    if (!this->astro) {
        return;
    }

    const double initSpectral = this->astro->getInitialSpectralValue();
    const double *axesInc = this->astro->getIncrements();

    QVector<double> key(this->Dimensions[2]), values(this->Dimensions[2]);
    for (vtkIdType k = 0; k < this->Dimensions[2]; ++k) {
        const vtkIdType idx = 1ll * pixX * this->Increments[0] + pixY * this->Increments[1]
                + k * this->Increments[2];
        key[k] = initSpectral + k * axesInc[2];
        values[k] = this->ScalarPointer[idx];
    }
    ui->plot1->graph()->setData(key, values);
    ui->plot1->rescaleAxes();
    qobject_cast<QCPTextElement *>(ui->plot1->plotLayout()->element(0, 0))
            ->setText(u"Z Profile (%1, %2)"_s.arg(pixX).arg(pixY));
    ui->plot1->replot();

    this->show();
    this->raise();
}

void ProfileWidget::updateSpectrumPlot(const QVector<double> &key, const QVector<double> &values,
                                       const QString &title, bool live)
{
    if (ui->plot1->graphCount() == 0) {
        ui->plot1->addGraph();
    }
    while (ui->plot1->graphCount() > 1) {
        ui->plot1->removeGraph(ui->plot1->graphCount() - 1);
    }
    ui->plot1->legend->setVisible(false);

    if (this->usageMode == UsageMode::ProbeLive) {
        ui->checkLive->setChecked(live);
    }
    ui->plot1->graph()->setPen(QPen(QColor(0, 170, 255), 1.5));
    ui->plot1->graph()->setData(key, values);
    ui->plot1->rescaleAxes();
    if (auto *titleElement = qobject_cast<QCPTextElement *>(ui->plot1->plotLayout()->element(0, 0))) {
        titleElement->setText(title);
    }
    ui->plot1->replot();
    this->show();
    this->raise();
}

void ProfileWidget::updateSpectrumPlotSeries(const QVector<double> &key,
                                             const QVector<double> &primaryValues,
                                             const QString &primaryLabel,
                                             const QVector<double> &secondaryValues,
                                             const QString &secondaryLabel,
                                             const QString &title, bool live)
{
    while (ui->plot1->graphCount() < 2) {
        ui->plot1->addGraph();
    }
    while (ui->plot1->graphCount() > 2) {
        ui->plot1->removeGraph(ui->plot1->graphCount() - 1);
    }

    if (this->usageMode == UsageMode::ProbeLive) {
        ui->checkLive->setChecked(live);
    }

    ui->plot1->graph(0)->setData(key, primaryValues);
    ui->plot1->graph(0)->setName(primaryLabel);
    ui->plot1->graph(0)->setPen(QPen(QColor(0, 170, 255), 2.0));
    ui->plot1->graph(1)->setData(key, secondaryValues);
    ui->plot1->graph(1)->setName(secondaryLabel);
    ui->plot1->graph(1)->setPen(QPen(QColor(255, 170, 0), 2.0));
    ui->plot1->legend->setVisible(true);
    ui->plot1->legend->setBrush(QColor(u"#1a2030"_s));
    ui->plot1->legend->setBorderPen(QPen(QColor(u"#2a3555"_s)));
    ui->plot1->legend->setTextColor(QColor(u"#c0d0f0"_s));
    ui->plot1->rescaleAxes();
    if (auto *titleElement = qobject_cast<QCPTextElement *>(ui->plot1->plotLayout()->element(0, 0))) {
        titleElement->setText(title);
    }
    ui->plot1->replot();
    this->show();
    this->raise();
}

void ProfileWidget::updateImageProfiles(const QVector<double> &keyX, const QVector<double> &valuesX,
                                        const QVector<double> &keyY, const QVector<double> &valuesY,
                                        double probeX, double probeY, bool live)
{
    if (ui->plot1->graphCount() == 0 || ui->plot2->graphCount() == 0) {
        this->setupImagePlots(u"X"_s, u"Value"_s);
    }

    if (this->usageMode == UsageMode::ProbeLive) {
        ui->checkLive->setChecked(live);
    }
    ui->plot1->graph()->setData(keyX, valuesX);
    ui->plot1->rescaleAxes();
    ui->plot1->xAxis->setRange(-1., keyX.size() + 1.);
    if (this->refLineX) {
        this->refLineX->start->setCoords(probeX, ui->plot1->yAxis->range().lower);
        this->refLineX->end->setCoords(probeX, ui->plot1->yAxis->range().upper);
    }
    if (auto *titleX = qobject_cast<QCPTextElement *>(ui->plot1->plotLayout()->element(0, 0))) {
        titleX->setText(u"X Profile"_s);
    }
    ui->plot1->replot();

    ui->plot2->graph()->setData(keyY, valuesY);
    ui->plot2->rescaleAxes();
    ui->plot2->xAxis->setRange(-1., keyY.size() + 1.);
    if (this->refLineY) {
        this->refLineY->start->setCoords(probeY, ui->plot2->yAxis->range().lower);
        this->refLineY->end->setCoords(probeY, ui->plot2->yAxis->range().upper);
    }
    if (auto *titleY = qobject_cast<QCPTextElement *>(ui->plot2->plotLayout()->element(0, 0))) {
        titleY->setText(u"Y Profile"_s);
    }
    ui->plot2->replot();

    this->show();
    this->raise();
}

void ProfileWidget::applyPlotTheme(bool dark)
{
    auto applyToPlot = [dark](QCustomPlot *plot) {
        if (!plot || !plot->isVisible()) return;
        if (dark) {
            plot->setBackground(QColor(u"#141922"_s));
            plot->axisRect()->setBackground(QColor(u"#0d1118"_s));
            if (auto *te = qobject_cast<QCPTextElement *>(plot->plotLayout()->element(0, 0)))
                te->setTextColor(QColor(u"#c8d8f8"_s));
            for (auto *axis : { plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2 }) {
                axis->setBasePen(QPen(QColor(u"#4a5a7a"_s)));
                axis->setTickPen(QPen(QColor(u"#4a5a7a"_s)));
                axis->setSubTickPen(QPen(QColor(u"#2a3555"_s)));
                axis->setTickLabelColor(QColor(u"#c0d0f0"_s));
                axis->setLabelColor(QColor(u"#8ab4f8"_s));
            }
        } else {
            plot->setBackground(Qt::white);
            plot->axisRect()->setBackground(QColor(u"#f8f8f8"_s));
            if (auto *te = qobject_cast<QCPTextElement *>(plot->plotLayout()->element(0, 0)))
                te->setTextColor(Qt::black);
            for (auto *axis : { plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2 }) {
                axis->setBasePen(QPen(Qt::black));
                axis->setTickPen(QPen(Qt::black));
                axis->setSubTickPen(QPen(QColor(u"#888888"_s)));
                axis->setTickLabelColor(Qt::black);
                axis->setLabelColor(Qt::black);
            }
        }
        plot->replot();
    };
    applyToPlot(ui->plot1);
    applyToPlot(ui->plot2);
}

void ProfileWidget::savePlot1Png()
{
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save Plot as PNG"_s, u"profile.png"_s, u"PNG Image (*.png)"_s);
    if (!path.isEmpty())
        ui->plot1->savePng(path);
}

void ProfileWidget::savePlot2Png()
{
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save Plot as PNG"_s, u"profile2.png"_s, u"PNG Image (*.png)"_s);
    if (!path.isEmpty())
        ui->plot2->savePng(path);
}
