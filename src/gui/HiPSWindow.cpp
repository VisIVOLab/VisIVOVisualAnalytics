#include "HiPSWindow.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;

// ── Free helpers ──────────────────────────────────────────────────────────────

static HiPSOpenTaskResult runHiPSOpenTask(const QString &backendUrl,
                                          const QString &backendToken,
                                          const QString &url)
{
    BackendClient client(backendUrl, backendToken);
    const BackendHiPSSurveyInfo info = client.openHiPS(url);
    HiPSOpenTaskResult r;
    r.valid  = info.valid;
    r.error  = info.error;
    r.survey = info;
    return r;
}

static QString fmtRa(double ra)
{
    const double h = ra / 15.0;
    const int    H = static_cast<int>(h);
    const double m = (h - H) * 60.0;
    const int    M = static_cast<int>(m);
    const double s = (m - M) * 60.0;
    return QStringLiteral("RA %1h%2m%3s")
            .arg(H, 2, 10, QLatin1Char('0'))
            .arg(M, 2, 10, QLatin1Char('0'))
            .arg(s, 4, 'f', 1, QLatin1Char('0'));
}

static QString fmtDec(double dec)
{
    const char sign = dec < 0 ? '-' : '+';
    const double a  = std::abs(dec);
    const int    D  = static_cast<int>(a);
    const double m  = (a - D) * 60.0;
    const int    M  = static_cast<int>(m);
    const double s  = (m - M) * 60.0;
    return QStringLiteral("Dec %1%2°%3'%4\"")
            .arg(sign)
            .arg(D, 2, 10, QLatin1Char('0'))
            .arg(M, 2, 10, QLatin1Char('0'))
            .arg(s, 4, 'f', 1, QLatin1Char('0'));
}

// ── Construction ──────────────────────────────────────────────────────────────

HiPSWindow::HiPSWindow(const QString &backendUrl, const QString &backendToken, QWidget *parent)
    : QWidget(parent)
    , m_backendUrl(backendUrl)
    , m_backendToken(backendToken)
{
    setupUi();
    connectViewport();

    QObject::connect(&m_openWatcher, &QFutureWatcher<HiPSOpenTaskResult>::finished,
                     this, &HiPSWindow::finishOpenSurvey);
}

// ── UI setup ──────────────────────────────────────────────────────────────────

void HiPSWindow::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Tool strip ────────────────────────────────────────────────────────
    auto *toolStrip = new QFrame(this);
    toolStrip->setFrameShape(QFrame::StyledPanel);
    toolStrip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *toolLayout = new QHBoxLayout(toolStrip);
    toolLayout->setContentsMargins(6, 3, 6, 3);
    toolLayout->setSpacing(4);

    auto *urlLabel = new QLabel(u"Survey URL:"_s, toolStrip);
    toolLayout->addWidget(urlLabel);

    m_urlEdit = new QLineEdit(toolStrip);
    m_urlEdit->setPlaceholderText(u"https://alasky.unistra.fr/DSS/DSSColor"_s);
    m_urlEdit->setMinimumWidth(360);
    m_urlEdit->setToolTip(u"Enter the base URL of a HiPS survey (must contain a 'properties' file)."_s);
    QObject::connect(m_urlEdit, &QLineEdit::returnPressed, this, &HiPSWindow::onOpenClicked);
    toolLayout->addWidget(m_urlEdit, 1);

    m_openBtn = new QPushButton(u"Open"_s, toolStrip);
    QObject::connect(m_openBtn, &QPushButton::clicked, this, &HiPSWindow::onOpenClicked);
    toolLayout->addWidget(m_openBtn);

    toolLayout->addSpacing(8);

    m_zoomOutBtn = new QToolButton(toolStrip);
    m_zoomOutBtn->setText(u"−"_s);
    m_zoomOutBtn->setToolTip(u"Zoom out"_s);
    QObject::connect(m_zoomOutBtn, &QToolButton::clicked, this, &HiPSWindow::onZoomOut);
    toolLayout->addWidget(m_zoomOutBtn);

    m_zoomInBtn = new QToolButton(toolStrip);
    m_zoomInBtn->setText(u"+"_s);
    m_zoomInBtn->setToolTip(u"Zoom in"_s);
    QObject::connect(m_zoomInBtn, &QToolButton::clicked, this, &HiPSWindow::onZoomIn);
    toolLayout->addWidget(m_zoomInBtn);

    m_resetBtn = new QToolButton(toolStrip);
    m_resetBtn->setText(u"⌂"_s);
    m_resetBtn->setToolTip(u"Reset view"_s);
    QObject::connect(m_resetBtn, &QToolButton::clicked, this, &HiPSWindow::onResetView);
    toolLayout->addWidget(m_resetBtn);

    toolLayout->addSpacing(8);

    m_orderLabel = new QLabel(u"Order: —"_s, toolStrip);
    m_orderLabel->setToolTip(u"Current HEALPix order (detail level)."_s);
    toolLayout->addWidget(m_orderLabel);

    root->addWidget(toolStrip);

    // ── Viewport ──────────────────────────────────────────────────────────
    m_viewport = new HiPSViewportWidget(this);
    root->addWidget(m_viewport, 1);

    // ── Status strip ──────────────────────────────────────────────────────
    auto *statusStrip = new QFrame(this);
    statusStrip->setFrameShape(QFrame::StyledPanel);
    statusStrip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *statusLayout = new QHBoxLayout(statusStrip);
    statusLayout->setContentsMargins(6, 2, 6, 2);
    statusLayout->setSpacing(12);

    m_statusMsgLabel = new QLabel(u"Open a HiPS survey to begin."_s, statusStrip);
    m_statusMsgLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusLayout->addWidget(m_statusMsgLabel, 1);

    auto *sep1 = new QFrame(statusStrip);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    statusLayout->addWidget(sep1);

    m_coordsLabel = new QLabel(u"RA — Dec —"_s, statusStrip);
    m_coordsLabel->setMinimumWidth(240);
    statusLayout->addWidget(m_coordsLabel);

    auto *sep2 = new QFrame(statusStrip);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setFrameShadow(QFrame::Sunken);
    statusLayout->addWidget(sep2);

    m_surveyLabel = new QLabel(u"No survey"_s, statusStrip);
    m_surveyLabel->setMinimumWidth(160);
    statusLayout->addWidget(m_surveyLabel);

    auto *sep3 = new QFrame(statusStrip);
    sep3->setFrameShape(QFrame::VLine);
    sep3->setFrameShadow(QFrame::Sunken);
    statusLayout->addWidget(sep3);

    m_fovLabel = new QLabel(u"FOV —"_s, statusStrip);
    m_fovLabel->setMinimumWidth(100);
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
}

// ── Status helpers ────────────────────────────────────────────────────────────

void HiPSWindow::showStatus(const QString &msg)
{
    if (m_statusMsgLabel)
        m_statusMsgLabel->setText(msg);
}

void HiPSWindow::clearStatus()
{
    if (m_statusMsgLabel)
        m_statusMsgLabel->clear();
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void HiPSWindow::onOpenClicked()
{
    const QString url = m_urlEdit->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, u"HiPS Viewer"_s,
                             u"Please enter a HiPS survey base URL."_s);
        return;
    }
    beginOpenSurvey(url);
}

void HiPSWindow::beginOpenSurvey(const QString &url)
{
    if (m_openWatcher.isRunning()) return;

    m_openBtn->setEnabled(false);
    showStatus(QStringLiteral("Opening survey: %1 …").arg(url));
    m_surveyLabel->setText(u"Loading…"_s);

    const QString backendUrl   = m_backendUrl;
    const QString backendToken = m_backendToken;
    m_openWatcher.setFuture(
            QtConcurrent::run([backendUrl, backendToken, url]() -> HiPSOpenTaskResult {
                return runHiPSOpenTask(backendUrl, backendToken, url);
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
    m_surveyLabel->setText(r.survey.title.left(30));
    showStatus(QStringLiteral("Survey loaded: %1  (max order %2, frame: %3)")
                       .arg(r.survey.title)
                       .arg(r.survey.maxOrder)
                       .arg(r.survey.frame));

    m_viewport->setSurvey(r.survey, m_backendUrl, m_backendToken);
    m_viewport->setFocus();
}

void HiPSWindow::onZoomIn()
{
    if (m_viewport) m_viewport->setFov(m_viewport->fov() * 0.75);
}

void HiPSWindow::onZoomOut()
{
    if (m_viewport) m_viewport->setFov(m_viewport->fov() * 1.333);
}

void HiPSWindow::onResetView()
{
    if (m_viewport) m_viewport->resetView();
}

void HiPSWindow::onCursorCoordinates(double ra, double dec)
{
    if (m_coordsLabel)
        m_coordsLabel->setText(QStringLiteral("%1   %2").arg(fmtRa(ra), fmtDec(dec)));
}

void HiPSWindow::onCenterChanged(double ra, double dec)
{
    Q_UNUSED(ra);
    Q_UNUSED(dec);
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
    if (loading)
        showStatus(u"Loading tiles…"_s);
    else
        clearStatus();
}

void HiPSWindow::openSurveyUrl(const QString &url)
{
    m_urlEdit->setText(url);
    QTimer::singleShot(200, this, [this, url]() { beginOpenSurvey(url); });
}
