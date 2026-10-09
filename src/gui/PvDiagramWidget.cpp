#include "PvDiagramWidget.h"

#include "ColorMaps.h"
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
#include <QEventLoop>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
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

    // ── Footer toolbar (Save PNG) ─────────────────────────────────────────────
    auto *footer = new QFrame(this);
    footer->setStyleSheet(u"QFrame { background: %1; border-top: 1px solid %2; }"_s
                                  .arg(tHeaderBg(), tBorder()));
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
        for (auto *axis : { this->plot->xAxis, this->plot->xAxis2,
                            this->plot->yAxis, this->plot->yAxis2 }) {
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
                                          ? u"-"_s : info.startPoint);
    if (m_lblEnd)      m_lblEnd->setText(info.endPoint.isEmpty()
                                        ? u"-"_s : info.endPoint);
    if (m_lblVertices) m_lblVertices->setText(QString::number(info.vertexCount));
    if (m_lblLength)   m_lblLength->setText(
                               QString::number(info.lengthPixels, 'f', 2) + u" px"_s);

    if (m_lblWidth)    m_lblWidth->setText(
                               QString::number(info.widthPixels) + u" px"_s);
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
