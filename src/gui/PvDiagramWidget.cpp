#include "PvDiagramWidget.h"

#include "ColorMaps.h"
#include "qcustomplot.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

// ── Style constants (match the cube-viewer sidebar palette) ───────────────────

static const QString kBg          = u"#141922"_s;
static const QString kHeaderBg    = u"#1a2030"_s;
static const QString kGroupBorder = u"rgba(4, 138, 191, 0.25)"_s;
static const QString kGroupTitle  = u"rgba(4, 138, 191, 0.90)"_s;
static const QString kKeyColor    = u"#7888a8"_s;
static const QString kValueColor  = u"#d0deff"_s;

// ─────────────────────────────────────────────────────────────────────────────

PvDiagramWidget::PvDiagramWidget(QWidget *parent)
    : QWidget(parent)
    , plot(new QCustomPlot(this))
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setWindowFlag(Qt::Window);
    this->setWindowTitle(u"PV Diagram"_s);
    this->resize(960, 700);
    this->setStyleSheet(u"background: %1;"_s.arg(kBg));

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

    // ── Footer toolbar (Save PNG) ─────────────────────────────────────────────
    auto *footer = new QFrame(this);
    footer->setStyleSheet(u"QFrame { background: %1; border-top: 1px solid rgba(4,138,191,0.15); }"_s.arg(kHeaderBg));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 10, 12, 10);

    auto *buttonBox = new QDialogButtonBox(footer);
    auto *btnSave = new QPushButton(u"Save as PNG\u2026"_s);
    btnSave->setToolTip(u"Save the PV diagram as a PNG image"_s);
    buttonBox->addButton(btnSave, QDialogButtonBox::ActionRole);
    footerLayout->addWidget(buttonBox);

    root->addWidget(footer);
    QObject::connect(btnSave, &QPushButton::clicked, this, &PvDiagramWidget::savePng);

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

    // Apply default dark theme.
    applyPlotTheme(true);
}

// ─────────────────────────────────────────────────────────────────────────────
// applyPlotTheme
// ─────────────────────────────────────────────────────────────────────────────

void PvDiagramWidget::applyPlotTheme(bool dark)
{
    if (dark) {
        this->plot->setBackground(QColor(kBg));
        this->plot->axisRect()->setBackground(QColor(u"#0d1118"_s));
        if (this->titleElement)
            this->titleElement->setTextColor(QColor(u"#c8d8f8"_s));
        for (auto *axis : { this->plot->xAxis, this->plot->xAxis2,
                            this->plot->yAxis, this->plot->yAxis2 }) {
            axis->setBasePen(QPen(QColor(u"#4a5a7a"_s)));
            axis->setTickPen(QPen(QColor(u"#4a5a7a"_s)));
            axis->setSubTickPen(QPen(QColor(u"#2a3555"_s)));
            axis->setTickLabelColor(QColor(u"#c0d0f0"_s));
            axis->setLabelColor(QColor(u"#8ab4f8"_s));
        }
    } else {
        this->plot->setBackground(Qt::white);
        this->plot->axisRect()->setBackground(QColor(u"#f8f8f8"_s));
        if (this->titleElement)
            this->titleElement->setTextColor(Qt::black);
        for (auto *axis : { this->plot->xAxis, this->plot->xAxis2,
                            this->plot->yAxis, this->plot->yAxis2 }) {
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
    const QString path = QFileDialog::getSaveFileName(
            this, u"Save PV Diagram as PNG"_s, u"pv_diagram.png"_s,
            u"PNG Image (*.png)"_s);
    if (path.isEmpty())
        return;
    this->plot->savePng(path);
}

// ─────────────────────────────────────────────────────────────────────────────
// buildHeaderUi — constructs the two-column info panel and adds it to the
// widget's top-level layout (which must already exist).
// ─────────────────────────────────────────────────────────────────────────────

void PvDiagramWidget::buildHeaderUi()
{
    auto *header = new QFrame(this);
    header->setStyleSheet(
            u"QFrame {"
            "  background: %1;"
            "  border-bottom: 1px solid rgba(4,138,191,0.20);"
            "}"_s.arg(kHeaderBg));

    auto *hLayout = new QHBoxLayout(header);
    hLayout->setContentsMargins(12, 8, 12, 8);
    hLayout->setSpacing(16);

    auto makeGroup = [&](const QString &title) -> QFormLayout * {
        auto *box = new QGroupBox(title, header);
        box->setStyleSheet(
                u"QGroupBox {"
                "  border: 1px solid %1;"
                "  border-radius: 4px;"
                "  margin-top: 6px;"
                "  color: %2;"
                "  font-weight: bold;"
                "  font-size: 10px;"
                "}"
                "QGroupBox::title {"
                "  subcontrol-origin: margin;"
                "  left: 8px;"
                "  padding: 0 3px;"
                "}"_s.arg(kGroupBorder, kGroupTitle));

        auto *form = new QFormLayout(box);
        form->setContentsMargins(8, 10, 8, 8);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(4);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        hLayout->addWidget(box, 1);
        return form;
    };

    auto makeRow = [&](QFormLayout *form,
                       const QString &key,
                       QLabel *&labelOut) {
        auto *keyLbl = new QLabel(key + u":"_s);
        keyLbl->setStyleSheet(u"color: %1; font-size: 10px;"_s.arg(kKeyColor));
        labelOut = new QLabel(u"—"_s);
        labelOut->setStyleSheet(u"color: %1; font-size: 10px;"_s.arg(kValueColor));
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
    auto *sampForm = makeGroup(u"Sampling & Axes"_s);
    makeRow(sampForm, u"Width"_s,    m_lblWidth);
    makeRow(sampForm, u"Spectral"_s, m_lblSpectral);
    makeRow(sampForm, u"Spatial"_s,  m_lblSpatial);
    makeRow(sampForm, u"Scale"_s,    m_lblScale);

    // Insert the header at position 0 in the widget's root layout.
    qobject_cast<QVBoxLayout *>(this->layout())->insertWidget(0, header);
}

void PvDiagramWidget::updateHeaderUi(const PvPathInfo &info)
{
    if (m_lblStart)    m_lblStart->setText(info.startPoint.isEmpty()
                                          ? u"—"_s : info.startPoint);
    if (m_lblEnd)      m_lblEnd->setText(info.endPoint.isEmpty()
                                        ? u"—"_s : info.endPoint);
    if (m_lblVertices) m_lblVertices->setText(QString::number(info.vertexCount));
    if (m_lblLength)   m_lblLength->setText(
                               QString::number(info.lengthPixels, 'f', 2) + u" px"_s);

    if (m_lblWidth)    m_lblWidth->setText(
                               QString::number(info.widthPixels) + u" px"_s);
    if (m_lblSpectral) m_lblSpectral->setText(
                               info.spectralAxisLabel.isEmpty()
                               ? u"—"_s : info.spectralAxisLabel);
    if (m_lblSpatial)  m_lblSpatial->setText(
                               info.spatialUnit.isEmpty()
                               ? u"pixels"_s : info.spatialUnit);
    if (m_lblScale) {
        if (info.pixelScaleArcsecPerPixel > 0.0)
            m_lblScale->setText(
                    QString::number(info.pixelScaleArcsecPerPixel, 'g', 6)
                    + u" arcsec/px"_s);
        else
            m_lblScale->setText(u"—"_s);
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
    this->titleElement->setText(title);
    this->plot->xAxis->setLabel(xLabel);
    this->plot->yAxis->setLabel(yLabel);

    const double xMin = positions.first();
    const double xMax = positions.last();
    const double yMin = *std::min_element(spectral.cbegin(), spectral.cend());
    const double yMax = *std::max_element(spectral.cbegin(), spectral.cend());

    this->colorMap->data()->setSize(nx, ny);
    this->colorMap->data()->setRange(QCPRange(xMin, xMax), QCPRange(yMin, yMax));

    double minValue = std::numeric_limits<double>::infinity();
    double maxValue = -std::numeric_limits<double>::infinity();
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const double value = intensities[y * nx + x];
            this->colorMap->data()->setCell(x, y, value);
            if (std::isfinite(value)) {
                minValue = std::min(minValue, value);
                maxValue = std::max(maxValue, value);
            }
        }
    }

    if (!std::isfinite(minValue) || !std::isfinite(maxValue) || minValue == maxValue) {
        minValue = 0.0;
        maxValue = minValue + 1.0;
    }

    this->colorMap->setDataRange(QCPRange(minValue, maxValue));
    this->colorMap->setGradient(QCPColorGradient::gpJet);
    this->plot->rescaleAxes();
    this->plot->replot();
    this->show();
    this->raise();
    this->activateWindow();
}
