#include "DataHubWidget.h"
#include "app/BackendClient.h"

#include <QDebug>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

DataHubWidget::DataHubWidget(QWidget *parent) : QWidget(parent)
{
    buildUi();
}

void DataHubWidget::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 20, 24, 20);
    rootLayout->setSpacing(16);

    // ── Title ────────────────────────────────────────────────────────────────
    auto *titleLabel = new QLabel(u"VisIVO Visual Analytics — Data Hub"_s, this);
    {
        QFont f = titleLabel->font();
        f.setPointSize(f.pointSize() + 5);
        f.setBold(true);
        titleLabel->setFont(f);
    }
    rootLayout->addWidget(titleLabel);

    auto *subtitleLabel = new QLabel(
            u"VisIVO uses a backend-authoritative workflow. "
            "Scientific operations are available for backend-visible datasets."_s,
            this);
    subtitleLabel->setWordWrap(true);
    subtitleLabel->setStyleSheet(u"color: palette(mid);"_s);
    rootLayout->addWidget(subtitleLabel);

    // ── Separator ────────────────────────────────────────────────────────────
    auto *sep1 = new QFrame(this);
    sep1->setFrameShape(QFrame::HLine);
    sep1->setFrameShadow(QFrame::Sunken);
    rootLayout->addWidget(sep1);

    // ── Content row: status + quick-actions + recent ─────────────────────────
    auto *contentRow = new QHBoxLayout;
    contentRow->setSpacing(20);
    rootLayout->addLayout(contentRow);

    // ── Backend status section ───────────────────────────────────────────────
    auto *statusGroup = new QGroupBox(u"Backend"_s, this);
    statusGroup->setMinimumWidth(240);
    statusGroup->setMaximumWidth(300);
    auto *statusLayout = new QVBoxLayout(statusGroup);
    statusLayout->setSpacing(8);

    auto *urlRow = new QHBoxLayout;
    urlRow->addWidget(new QLabel(u"URL:"_s));
    this->labelBackendUrl = new QLabel(u"(not configured)"_s);
    this->labelBackendUrl->setWordWrap(true);
    this->labelBackendUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    urlRow->addWidget(this->labelBackendUrl, 1);
    statusLayout->addLayout(urlRow);

    auto *statusRow = new QHBoxLayout;
    // Colour dot — updated by refreshStatus()
    this->labelStatusIndicator = new QLabel(u"●"_s);
    this->labelStatusIndicator->setStyleSheet(u"color: gray; font-size: 18px;"_s);
    statusRow->addWidget(this->labelStatusIndicator);
    this->labelStatusText = new QLabel(u"Status unknown"_s);
    statusRow->addWidget(this->labelStatusText, 1);
    statusLayout->addLayout(statusRow);

    this->btnCheckConnection = new QPushButton(u"Check Connection"_s, statusGroup);
    this->btnCheckConnection->setToolTip(u"Send a health-check request to the configured backend URL."_s);
    statusLayout->addWidget(this->btnCheckConnection);
    statusLayout->addStretch(1);

    contentRow->addWidget(statusGroup);

    // ── Quick actions section ─────────────────────────────────────────────────
    auto *actionsGroup = new QGroupBox(u"Open Dataset"_s, this);
    auto *actionsLayout = new QVBoxLayout(actionsGroup);
    actionsLayout->setSpacing(10);

    this->btnOpenData = new QPushButton(u"Open Remote Dataset…"_s, actionsGroup);
    this->btnOpenData->setToolTip(u"Browse the backend filesystem and open a FITS image or spectral cube."_s);
    this->btnOpenData->setMinimumHeight(36);

    this->btnOpenCatalogue = new QPushButton(u"Open 3D Catalogue (CSV)…"_s, actionsGroup);
    this->btnOpenCatalogue->setToolTip(u"Open a local CSV catalogue in the 3D catalogue viewer."_s);
    this->btnOpenCatalogue->setMinimumHeight(36);

    this->btnOpenVbt = new QPushButton(u"Open VBT…"_s, actionsGroup);
    this->btnOpenVbt->setToolTip(u"Open a VisIVO Binary Table (.bin.head) dataset."_s);
    this->btnOpenVbt->setMinimumHeight(36);

    this->btnOpenHiPS = new QPushButton(u"HiPS Viewer  (coming soon)"_s, actionsGroup);
    this->btnOpenHiPS->setToolTip(u"Open the HiPS panoramic sky viewer."_s);
    this->btnOpenHiPS->setMinimumHeight(36);

    actionsLayout->addWidget(this->btnOpenData);
    actionsLayout->addWidget(this->btnOpenCatalogue);
    actionsLayout->addWidget(this->btnOpenVbt);
    actionsLayout->addWidget(this->btnOpenHiPS);
    actionsLayout->addStretch(1);

    contentRow->addWidget(actionsGroup, 1);

    // ── Recent / active datasets section ─────────────────────────────────────
    auto *recentGroup = new QGroupBox(u"Recent Datasets"_s, this);
    auto *recentLayout = new QVBoxLayout(recentGroup);

    this->listRecentDatasets = new QListWidget(recentGroup);
    this->listRecentDatasets->setSelectionMode(QAbstractItemView::NoSelection);
    this->listRecentDatasets->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Placeholder empty-state message
    auto *emptyItem = new QListWidgetItem(
            u"No recently opened datasets.\nOpen a dataset using the actions on the left."_s,
            this->listRecentDatasets);
    emptyItem->setFlags(Qt::NoItemFlags);
    emptyItem->setForeground(Qt::gray);
    recentLayout->addWidget(this->listRecentDatasets, 1);
    contentRow->addWidget(recentGroup, 1);

    rootLayout->addStretch(1);

    // ── Signal wiring ─────────────────────────────────────────────────────────
    QObject::connect(this->btnCheckConnection, &QPushButton::clicked,
                     this, &DataHubWidget::refreshStatus);
    QObject::connect(this->btnOpenData, &QPushButton::clicked,
                     this, &DataHubWidget::openDataRequested);
    QObject::connect(this->btnOpenCatalogue, &QPushButton::clicked,
                     this, &DataHubWidget::openCatalogueRequested);
    QObject::connect(this->btnOpenVbt, &QPushButton::clicked,
                     this, &DataHubWidget::openVbtRequested);
    QObject::connect(this->btnOpenHiPS, &QPushButton::clicked,
                     this, &DataHubWidget::openHiPSRequested);
}

void DataHubWidget::setBackendUrl(const QString &url, const QString &token)
{
    this->backendUrl   = url;
    this->backendToken = token;
    if (this->labelBackendUrl) {
        this->labelBackendUrl->setText(url.isEmpty() ? u"(not configured)"_s : url);
    }
    if (this->labelStatusIndicator) {
        this->labelStatusIndicator->setStyleSheet(u"color: gray; font-size: 18px;"_s);
    }
    if (this->labelStatusText) {
        this->labelStatusText->setText(u"Status unknown"_s);
    }
}

void DataHubWidget::refreshStatus()
{
    if (this->backendUrl.isEmpty()) {
        if (this->labelStatusIndicator) {
            this->labelStatusIndicator->setStyleSheet(u"color: orange; font-size: 18px;"_s);
        }
        if (this->labelStatusText) {
            this->labelStatusText->setText(u"No backend URL configured.\nSet it in Edit → Settings."_s);
        }
        return;
    }

    if (this->btnCheckConnection) {
        this->btnCheckConnection->setEnabled(false);
        this->btnCheckConnection->setText(u"Checking…"_s);
    }
    if (this->labelStatusText) {
        this->labelStatusText->setText(u"Checking…"_s);
    }

    // Synchronous health check — acceptable on the main thread for a manual
    // button press; the BackendClient already uses a short timeout.
    BackendClient client(this->backendUrl, this->backendToken);
    const BackendHealthResult health = client.health();

    if (health.ok) {
        if (this->labelStatusIndicator) {
            this->labelStatusIndicator->setStyleSheet(u"color: #3cb371; font-size: 18px;"_s);
        }
        if (this->labelStatusText) {
            const QString detail =
                    (health.workers > 0 || health.activeSessions > 0)
                    ? QStringLiteral("Connected  (workers: %1, sessions: %2)")
                              .arg(health.workers)
                              .arg(health.activeSessions)
                    : u"Connected"_s;
            this->labelStatusText->setText(detail);
        }
        qDebug().noquote()
                << QStringLiteral("[DataHub] backend reachable — workers=%1 sessions=%2")
                           .arg(health.workers)
                           .arg(health.activeSessions);
    } else {
        if (this->labelStatusIndicator) {
            this->labelStatusIndicator->setStyleSheet(u"color: #e05050; font-size: 18px;"_s);
        }
        if (this->labelStatusText) {
            this->labelStatusText->setText(
                    u"Not reachable\n"_s
                    + (health.error.isEmpty() ? u"No response from backend."_s : health.error));
        }
        qDebug().noquote()
                << QStringLiteral("[DataHub] backend unreachable — %1").arg(health.error);
    }

    if (this->btnCheckConnection) {
        this->btnCheckConnection->setEnabled(true);
        this->btnCheckConnection->setText(u"Check Connection"_s);
    }
}
