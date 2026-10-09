#include "HiPSWindow.h"

#include "HiPSCatalogueDialog.h"
#include "HiPSSurveyBrowserDialog.h"
#include "RemoteFileBrowserDialog.h"

#include <QComboBox>
#include <QSignalBlocker>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>

using namespace Qt::StringLiterals;

// ── Free helpers ──────────────────────────────────────────────────────────────

static HiPSOpenTaskResult runHiPSOpenTask(const QString &backendUrl,
                                          const QString &backendToken,
                                          const QString &url)
{
    BackendClient client(backendUrl, backendToken);
    const BackendHiPSSurveyInfo info = client.openHiPS(url);
    return { info.valid, info.error, info };
}

// Sexagesimal with the carry done before formatting: rounding the seconds to
// one decimal first is what turns 41d 29' 59.97" into 41d 30' 00.0" rather
// than the impossible 41d 29' 60.0" the status strip used to show.
static void sexagesimal(double value, int &whole, int &minutes, double &seconds)
{
    whole   = static_cast<int>(value);
    double m = (value - whole) * 60.0;
    minutes = static_cast<int>(m);
    seconds = std::round((m - minutes) * 60.0 * 10.0) / 10.0;
    if (seconds >= 60.0) { seconds -= 60.0; ++minutes; }
    if (minutes >= 60)   { minutes -= 60;   ++whole;   }
}

static QString fmtRa(double ra)
{
    int h = 0, m = 0; double sec = 0.0;
    sexagesimal(ra / 15.0, h, m, sec);
    if (h >= 24) h -= 24;
    return QStringLiteral("RA %1h%2m%3s")
            .arg(h, 2, 10, QLatin1Char('0'))
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(sec, 4, 'f', 1, QLatin1Char('0'));
}

static QString fmtDec(double dec)
{
    const char sign = dec < 0 ? '-' : '+';
    int d = 0, m = 0; double sec = 0.0;
    sexagesimal(std::abs(dec), d, m, sec);
    return QStringLiteral("Dec %1%2°%3'%4\"")
            .arg(sign)
            .arg(d, 2, 10, QLatin1Char('0'))
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(sec, 4, 'f', 1, QLatin1Char('0'));
}

// ── Construction ──────────────────────────────────────────────────────────────

HiPSWindow::HiPSWindow(const QString &backendUrl, const QString &backendToken, QWidget *parent)
    : QWidget(parent)
    , m_backendUrl(backendUrl)
    , m_backendToken(backendToken)
{
    setupUi();
    connectViewport();

    // Primary survey open watcher
    QObject::connect(&m_openWatcher, &QFutureWatcher<HiPSOpenTaskResult>::finished,
                     this, &HiPSWindow::finishOpenSurvey);

    // Secondary survey open watcher (Task 2)
    QObject::connect(&m_openWatcherSec, &QFutureWatcher<HiPSOpenTaskResult>::finished,
                     this, &HiPSWindow::finishOpenSurveySec);

    // Target resolver watcher (Task 4)
    QObject::connect(&m_resolveWatcher, &QFutureWatcher<BackendTargetResolveResult>::finished,
                     this, &HiPSWindow::onResolveFinished);
}

void HiPSWindow::setCenter(double raDeg, double decDeg)
{
    if (!m_viewport) {
        return;
    }
    m_viewport->setCenter(raDeg, decDeg);
    if (m_coordsLabel) {
        m_coordsLabel->setText(QStringLiteral("%1  %2").arg(fmtRa(raDeg), fmtDec(decDeg)));
    }
    showStatus(QStringLiteral("Centred on SAMP sky coordinates."));
}

// ── UI setup ──────────────────────────────────────────────────────────────────

void HiPSWindow::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ─── Strip 1 — primary survey URL + view controls + overlay toggles ───
    auto *strip1 = new QFrame(this);
    strip1->setFrameShape(QFrame::StyledPanel);
    strip1->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *lay1 = new QHBoxLayout(strip1);
    lay1->setContentsMargins(6, 3, 6, 3);
    lay1->setSpacing(4);

    lay1->addWidget(new QLabel(u"Survey:"_s, strip1));

    m_urlEdit = new QLineEdit(strip1);
    m_urlEdit->setPlaceholderText(u"https://alasky.unistra.fr/DSS/DSSColor"_s);
    m_urlEdit->setMinimumWidth(300);
    m_urlEdit->setToolTip(u"HiPS survey base URL (must contain a 'properties' file)."_s);
    QObject::connect(m_urlEdit, &QLineEdit::returnPressed, this, &HiPSWindow::onOpenClicked);
    lay1->addWidget(m_urlEdit, 1);

    // A URL you have to know is not a way in. The registry button lists every
    // image HiPS the CDS MOCServer publishes.
    m_browseBtn = new QPushButton(u"Browse…"_s, strip1);
    m_browseBtn->setToolTip(u"Choose from the surveys registered with CDS"_s);
    QObject::connect(m_browseBtn, &QPushButton::clicked, this, &HiPSWindow::onBrowseSurveys);
    lay1->addWidget(m_browseBtn);

    m_openBtn = new QPushButton(u"Open"_s, strip1);
    QObject::connect(m_openBtn, &QPushButton::clicked, this, &HiPSWindow::onOpenClicked);
    lay1->addWidget(m_openBtn);

    lay1->addSpacing(6);

    m_zoomOutBtn = new QToolButton(strip1);
    m_zoomOutBtn->setText(u"−"_s);
    m_zoomOutBtn->setToolTip(u"Zoom out"_s);
    QObject::connect(m_zoomOutBtn, &QToolButton::clicked, this, &HiPSWindow::onZoomOut);
    lay1->addWidget(m_zoomOutBtn);

    m_zoomInBtn = new QToolButton(strip1);
    m_zoomInBtn->setText(u"+"_s);
    m_zoomInBtn->setToolTip(u"Zoom in"_s);
    QObject::connect(m_zoomInBtn, &QToolButton::clicked, this, &HiPSWindow::onZoomIn);
    lay1->addWidget(m_zoomInBtn);

    m_resetBtn = new QToolButton(strip1);
    m_resetBtn->setText(u"⌂"_s);
    m_resetBtn->setToolTip(u"Reset view"_s);
    QObject::connect(m_resetBtn, &QToolButton::clicked, this, &HiPSWindow::onResetView);
    lay1->addWidget(m_resetBtn);

    m_orderLabel = new QLabel(u"Order: -"_s, strip1);
    lay1->addWidget(m_orderLabel);

    lay1->addSpacing(8);

    // Projection. Gnomonic shows a field; the all-sky ones put the whole
    // sphere on screen, which the viewer could not do at all before.
    m_projectionCombo = new QComboBox(strip1);
    m_projectionCombo->setToolTip(u"Sky projection"_s);
    for (auto proj : { SkyProjection::Tan, SkyProjection::Sin, SkyProjection::Arc,
                       SkyProjection::Ait, SkyProjection::Mol, SkyProjection::Mer }) {
        m_projectionCombo->addItem(HiPSViewportWidget::projectionCode(proj),
                                   QVariant::fromValue(static_cast<int>(proj)));
        m_projectionCombo->setItemData(m_projectionCombo->count() - 1,
                                       HiPSViewportWidget::projectionName(proj),
                                       Qt::ToolTipRole);
    }
    m_projectionCombo->setCurrentIndex(0);
    QObject::connect(m_projectionCombo, &QComboBox::currentIndexChanged,
                     this, &HiPSWindow::onProjectionSelected);
    lay1->addWidget(m_projectionCombo);

    lay1->addSpacing(8);

    // Task 1 — Grid toggle
    m_gridBtn = new QToolButton(strip1);
    m_gridBtn->setText(u"Grid"_s);
    m_gridBtn->setToolTip(u"Toggle RA/Dec coordinate grid"_s);
    m_gridBtn->setCheckable(true);
    m_gridBtn->setChecked(false);
    QObject::connect(m_gridBtn, &QToolButton::toggled, this, &HiPSWindow::onGridToggled);
    lay1->addWidget(m_gridBtn);

    // Task 3 — Catalogue overlay toggle
    m_catBtn = new QToolButton(strip1);
    m_catBtn->setText(u"Cat"_s);
    m_catBtn->setToolTip(u"Load/toggle catalogue overlay"_s);
    m_catBtn->setCheckable(true);
    m_catBtn->setChecked(false);
    QObject::connect(m_catBtn, &QToolButton::clicked, this, &HiPSWindow::onLoadCatalogueClicked);
    lay1->addWidget(m_catBtn);

    root->addWidget(strip1);

    // ─── Strip 2 — target resolver (Task 4) + secondary survey (Task 2) ──
    auto *strip2 = new QFrame(this);
    strip2->setFrameShape(QFrame::StyledPanel);
    strip2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *lay2 = new QHBoxLayout(strip2);
    lay2->setContentsMargins(6, 3, 6, 3);
    lay2->setSpacing(4);

    // Task 4 – Target resolver
    lay2->addWidget(new QLabel(u"Target:"_s, strip2));

    m_targetEdit = new QLineEdit(strip2);
    m_targetEdit->setPlaceholderText(u"M31, Cas A, NGC 1952, …"_s);
    m_targetEdit->setMinimumWidth(180);
    m_targetEdit->setToolTip(u"Enter object name to resolve (Sesame / SIMBAD)."_s);
    QObject::connect(m_targetEdit, &QLineEdit::returnPressed, this, &HiPSWindow::onResolveClicked);
    lay2->addWidget(m_targetEdit, 1);

    m_resolveBtn = new QPushButton(u"Go"_s, strip2);
    m_resolveBtn->setToolTip(u"Resolve target name and centre view"_s);
    QObject::connect(m_resolveBtn, &QPushButton::clicked, this, &HiPSWindow::onResolveClicked);
    lay2->addWidget(m_resolveBtn);

    auto *sep = new QFrame(strip2);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    lay2->addWidget(sep);

    // Task 2 – Secondary survey
    lay2->addWidget(new QLabel(u"Overlay:"_s, strip2));

    m_urlSecEdit = new QLineEdit(strip2);
    m_urlSecEdit->setPlaceholderText(u"https://alasky.unistra.fr/2MASS/J"_s);
    m_urlSecEdit->setMinimumWidth(220);
    m_urlSecEdit->setToolTip(u"URL of a second HiPS survey to overlay"_s);
    QObject::connect(m_urlSecEdit, &QLineEdit::returnPressed,
                     this, &HiPSWindow::onOpenSecondaryClicked);
    lay2->addWidget(m_urlSecEdit, 1);

    m_openSecBtn = new QPushButton(u"Open"_s, strip2);
    QObject::connect(m_openSecBtn, &QPushButton::clicked,
                     this, &HiPSWindow::onOpenSecondaryClicked);
    lay2->addWidget(m_openSecBtn);

    // Opacity slider (0–100 %)
    m_opacitySlider = new QSlider(Qt::Horizontal, strip2);
    m_opacitySlider->setRange(0, 100);
    m_opacitySlider->setValue(50);
    m_opacitySlider->setFixedWidth(80);
    m_opacitySlider->setToolTip(u"Overlay opacity"_s);
    QObject::connect(m_opacitySlider, &QSlider::valueChanged,
                     this, &HiPSWindow::onOpacityChanged);
    lay2->addWidget(m_opacitySlider);

    // Side-by-side toggle
    m_sideBySideBtn = new QToolButton(strip2);
    m_sideBySideBtn->setText(u"⧸⧸"_s);
    m_sideBySideBtn->setToolTip(u"Side-by-side comparison mode"_s);
    m_sideBySideBtn->setCheckable(true);
    m_sideBySideBtn->setChecked(false);
    QObject::connect(m_sideBySideBtn, &QToolButton::toggled,
                     this, &HiPSWindow::onBlendModeToggled);
    lay2->addWidget(m_sideBySideBtn);

    root->addWidget(strip2);

    // ─── Viewport ─────────────────────────────────────────────────────────
    m_viewport = new HiPSViewportWidget(this);
    root->addWidget(m_viewport, 1);

    // ─── Status strip ──────────────────────────────────────────────────────
    auto *statusStrip = new QFrame(this);
    statusStrip->setFrameShape(QFrame::StyledPanel);
    statusStrip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *statusLayout = new QHBoxLayout(statusStrip);
    statusLayout->setContentsMargins(6, 2, 6, 2);
    statusLayout->setSpacing(10);

    m_statusMsgLabel = new QLabel(u"Open a HiPS survey to begin."_s, statusStrip);
    m_statusMsgLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusLayout->addWidget(m_statusMsgLabel, 1);

    auto makeSep = [&]() {
        auto *s = new QFrame(statusStrip);
        s->setFrameShape(QFrame::VLine);
        s->setFrameShadow(QFrame::Sunken);
        return s;
    };

    statusLayout->addWidget(makeSep());
    m_coordsLabel = new QLabel(u"RA, Dec -"_s, statusStrip);
    m_coordsLabel->setMinimumWidth(220);
    statusLayout->addWidget(m_coordsLabel);
    statusLayout->addWidget(makeSep());
    m_surveyLabel = new QLabel(u"No survey"_s, statusStrip);
    m_surveyLabel->setMinimumWidth(140);
    statusLayout->addWidget(m_surveyLabel);
    statusLayout->addWidget(makeSep());
    m_fovLabel = new QLabel(u"FOV -"_s, statusStrip);
    m_fovLabel->setMinimumWidth(90);
    statusLayout->addWidget(m_fovLabel);

    root->addWidget(statusStrip);
}

void HiPSWindow::connectViewport()
{
    QObject::connect(m_viewport, &HiPSViewportWidget::cursorCoordinatesChanged,
                     this, &HiPSWindow::onCursorCoordinates);
    QObject::connect(m_viewport, &HiPSViewportWidget::centerChanged,
                     this, &HiPSWindow::onCenterChanged);
    QObject::connect(m_viewport, &HiPSViewportWidget::fovChanged,
                     this, &HiPSWindow::onFovChanged);
    QObject::connect(m_viewport, &HiPSViewportWidget::orderChanged,
                     this, &HiPSWindow::onOrderChanged);
    QObject::connect(m_viewport, &HiPSViewportWidget::loadingChanged,
                     this, &HiPSWindow::onLoadingChanged);
    QObject::connect(m_viewport, &HiPSViewportWidget::catalogueOverlayRefreshed,
                     this, &HiPSWindow::onCatalogueOverlayRefreshed);
    QObject::connect(m_viewport, &HiPSViewportWidget::catalogueOverlayFailed,
                     this, &HiPSWindow::onCatalogueOverlayFailed);
    // Keep the selector honest if the viewport ever changes projection itself.
    QObject::connect(m_viewport, &HiPSViewportWidget::projectionChanged,
                     this, [this](SkyProjection proj) {
                         if (!m_projectionCombo) return;
                         const int i = m_projectionCombo->findData(static_cast<int>(proj));
                         if (i < 0 || i == m_projectionCombo->currentIndex()) return;
                         const QSignalBlocker blocker(m_projectionCombo);
                         m_projectionCombo->setCurrentIndex(i);
                     });
}

// ── Status helpers ────────────────────────────────────────────────────────────

void HiPSWindow::showStatus(const QString &msg)
{
    if (m_statusMsgLabel) m_statusMsgLabel->setText(msg);
}

void HiPSWindow::clearStatus()
{
    if (m_statusMsgLabel) m_statusMsgLabel->clear();
}

// ── Primary survey open ───────────────────────────────────────────────────────

void HiPSWindow::onOpenClicked()
{
    const QString url = m_urlEdit->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, u"HiPS Viewer"_s,
                             u"Please enter a HiPS survey base URL."_s);
        return;
    }
    beginOpenSurvey(url, false);
}

void HiPSWindow::beginOpenSurvey(const QString &url, bool secondary)
{
    auto &watcher = secondary ? m_openWatcherSec : m_openWatcher;
    if (watcher.isRunning()) return;

    if (!secondary) {
        m_openBtn->setEnabled(false);
        m_surveyLabel->setText(u"Loading…"_s);
    } else {
        m_openSecBtn->setEnabled(false);
    }
    showStatus(QStringLiteral("Opening survey: %1 …").arg(url));

    const QString bUrl  = m_backendUrl;
    const QString bTok  = m_backendToken;
    watcher.setFuture(QtConcurrent::run(
            [bUrl, bTok, url]() -> HiPSOpenTaskResult {
                return runHiPSOpenTask(bUrl, bTok, url);
            }));
}

void HiPSWindow::finishOpenSurvey()
{
    m_openBtn->setEnabled(true);
    const HiPSOpenTaskResult r = m_openWatcher.result();

    if (!r.valid) {
        showStatus(QStringLiteral("Failed: %1").arg(r.error));
        QMessageBox::critical(this, u"HiPS Open Error"_s,
                              QStringLiteral("Could not open HiPS survey:\n\n%1").arg(r.error));
        return;
    }

    m_currentSurvey = r.survey;
    m_surveyLabel->setText(r.survey.title.left(28));
    showStatus(QStringLiteral("Survey: %1  (max order %2, frame: %3)")
                       .arg(r.survey.title).arg(r.survey.maxOrder).arg(r.survey.frame));

    m_viewport->setSurvey(r.survey, m_backendUrl, m_backendToken);
    m_viewport->setFocus();
}

void HiPSWindow::finishOpenSurveySec()
{
    m_openSecBtn->setEnabled(true);
    const HiPSOpenTaskResult r = m_openWatcherSec.result();

    if (!r.valid) {
        showStatus(QStringLiteral("Overlay failed: %1").arg(r.error));
        QMessageBox::critical(this, u"HiPS Overlay Error"_s,
                              QStringLiteral("Could not open secondary survey:\n\n%1").arg(r.error));
        return;
    }

    m_currentSurveySec = r.survey;
    showStatus(QStringLiteral("Overlay: %1").arg(r.survey.title));
    m_viewport->setSecondarySurvey(r.survey, m_backendUrl, m_backendToken);
}

// ── Task 1 — Grid toggle ──────────────────────────────────────────────────────

void HiPSWindow::onGridToggled(bool checked)
{
    if (m_viewport) m_viewport->setGridVisible(checked);
}

// ── Task 2 — Secondary survey controls ───────────────────────────────────────

void HiPSWindow::onOpenSecondaryClicked()
{
    const QString url = m_urlSecEdit->text().trimmed();
    if (url.isEmpty()) {
        // If empty, clear secondary survey
        if (m_viewport) m_viewport->clearSecondarySurvey();
        showStatus(u"Secondary survey cleared."_s);
        return;
    }
    beginOpenSurvey(url, true);
}

void HiPSWindow::onOpacityChanged(int value)
{
    if (m_viewport) m_viewport->setOverlayOpacity(value / 100.0);
}

void HiPSWindow::onBlendModeToggled(bool sideBySide)
{
    if (m_viewport)
        m_viewport->setBlendMode(sideBySide
                                  ? HiPSViewportWidget::BlendMode::SideBySide
                                  : HiPSViewportWidget::BlendMode::Alpha);
}

// ── Task 3 — Catalogue overlay ────────────────────────────────────────────────

void HiPSWindow::onLoadCatalogueClicked()
{
    if (!m_viewport) return;

    if (m_catBtn->isChecked() && m_viewport->hasCatalogueOverlay()) {
        // Already loaded: the button is a visibility toggle from here on.
        m_viewport->setCatalogueOverlayVisible(true);
        return;
    }
    if (!m_catBtn->isChecked()) {
        m_viewport->setCatalogueOverlayVisible(false);
        return;
    }

    HiPSCatalogueDialog dialog(m_backendUrl, m_backendToken,
                               m_viewport->centerRa(), m_viewport->centerDec(),
                               m_viewport->fov(), this);
    if (dialog.exec() != QDialog::Accepted || dialog.cataloguePath().isEmpty()) {
        m_catBtn->setChecked(false);
        return;
    }

    m_viewport->setCatalogueOverlay(dialog.cataloguePath(),
                                    dialog.raField(), dialog.decField());
    m_viewport->setCatalogueOverlayVisible(true);
    showStatus(QStringLiteral("Catalogue overlay: %1").arg(dialog.describeSource()));
}

void HiPSWindow::onBrowseSurveys()
{
    HiPSSurveyBrowserDialog dialog(m_backendUrl, m_backendToken, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString url = dialog.selectedUrl();
    if (url.isEmpty()) return;
    if (m_urlEdit) m_urlEdit->setText(url);
    beginOpenSurvey(url, false);
}

void HiPSWindow::onProjectionSelected(int index)
{
    if (!m_viewport || index < 0 || !m_projectionCombo) return;
    const auto proj = static_cast<SkyProjection>(
            m_projectionCombo->itemData(index).toInt());
    m_viewport->setProjection(proj);
    showStatus(QStringLiteral("Projection: %1")
                       .arg(HiPSViewportWidget::projectionName(proj)));
}

void HiPSWindow::onCatalogueOverlayRefreshed(int totalInView, bool truncated)
{
    const QString msg = truncated
            ? QStringLiteral("Catalogue: %1 sources (capped; %2 in view).")
                      .arg(5000).arg(totalInView)
            : QStringLiteral("Catalogue: %1 sources in view.").arg(totalInView);
    showStatus(msg);
}

// ── Task 4 — Target resolver ──────────────────────────────────────────────────

void HiPSWindow::onCatalogueOverlayFailed(const QString &error)
{
    // Put the button back so the next click offers the picker again rather
    // than toggling the visibility of a catalogue that was never loaded.
    if (m_catBtn) m_catBtn->setChecked(false);
    showStatus(QStringLiteral("Catalogue overlay: %1").arg(error));
}

void HiPSWindow::onResolveClicked()
{
    const QString name = m_targetEdit->text().trimmed();
    if (name.isEmpty()) return;
    if (m_resolveWatcher.isRunning()) return;

    m_resolveBtn->setEnabled(false);
    showStatus(QStringLiteral("Resolving \"%1\"...").arg(name));

    const QString bUrl = m_backendUrl;
    const QString bTok = m_backendToken;
    m_resolveWatcher.setFuture(QtConcurrent::run(
            [bUrl, bTok, name]() -> BackendTargetResolveResult {
                BackendClient client(bUrl, bTok);
                return client.resolveTarget(name);
            }));
}

void HiPSWindow::onResolveFinished()
{
    m_resolveBtn->setEnabled(true);
    const BackendTargetResolveResult r = m_resolveWatcher.result();

    if (!r.valid) {
        showStatus(QStringLiteral("Resolve failed: %1").arg(r.error));
        QMessageBox::warning(this, u"Target Resolver"_s,
                             QStringLiteral("Could not resolve target \"%1\":\n\n%2")
                                     .arg(m_targetEdit->text().trimmed(), r.error));
        return;
    }

    showStatus(QStringLiteral("Resolved: %1  →  %2   %3   (via %4)")
                       .arg(r.resolvedName, fmtRa(r.raDeg), fmtDec(r.decDeg), r.source));

    if (m_viewport) {
        m_viewport->setCenter(r.raDeg, r.decDeg);
        // Zoom in to 1° if currently zoomed far out
        if (m_viewport->fov() > 10.0)
            m_viewport->setFov(1.0);
    }
}

// ── Viewport signal slots ─────────────────────────────────────────────────────

void HiPSWindow::onCursorCoordinates(double ra, double dec)
{
    if (m_coordsLabel)
        m_coordsLabel->setText(QStringLiteral("%1   %2").arg(fmtRa(ra), fmtDec(dec)));
}

void HiPSWindow::onCenterChanged(double ra, double dec)
{
    // Show the view centre whenever it moves. A cursor over the viewport
    // overwrites this with the position under the pointer, which is what you
    // want while pointing; without it the label read "-" until the first
    // mouse-move, even after a resolved target had recentred the view.
    if (m_coordsLabel)
        m_coordsLabel->setText(QStringLiteral("%1   %2").arg(fmtRa(ra), fmtDec(dec)));
}

void HiPSWindow::onFovChanged(double fov)
{
    if (m_fovLabel)
        m_fovLabel->setText(QStringLiteral("FOV %1°").arg(fov, 0, 'f', fov < 1.0 ? 3 : 1));
}

void HiPSWindow::onOrderChanged(int order)
{
    if (m_orderLabel)
        m_orderLabel->setText(QStringLiteral("Order: %1").arg(order));
}

void HiPSWindow::onLoadingChanged(bool loading)
{
    if (loading) showStatus(u"Loading tiles…"_s);
    else         clearStatus();
}

void HiPSWindow::onZoomIn()  { if (m_viewport) m_viewport->setFov(m_viewport->fov() * 0.75); }
void HiPSWindow::onZoomOut() { if (m_viewport) m_viewport->setFov(m_viewport->fov() * 1.333); }
void HiPSWindow::onResetView() { if (m_viewport) m_viewport->resetView(); }

QString HiPSWindow::fmtRa(double ra) const  { return ::fmtRa(ra); }
QString HiPSWindow::fmtDec(double dec) const { return ::fmtDec(dec); }

void HiPSWindow::openSurveyUrl(const QString &url)
{
    m_urlEdit->setText(url);
    QTimer::singleShot(200, this, [this, url]() { beginOpenSurvey(url, false); });
}
