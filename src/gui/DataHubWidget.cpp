#include "DataHubWidget.h"
#include "app/BackendClient.h"
#include "theme/VisivoTheme.h"

#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

// ---------------------------------------------------------------------------
// CardClickFilter
// Installed on a QFrame to emit a signal when the user left-clicks the card.
// Uses a lambda to avoid a separate MOC-registered class.
// ---------------------------------------------------------------------------
class CardClickFilter : public QObject
{
public:
    std::function<void()> onClicked;
    explicit CardClickFilter(std::function<void()> fn, QObject *parent)
        : QObject(parent), onClicked(std::move(fn))
    {}
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                if (auto *w = qobject_cast<QWidget *>(obj)) {
                    if (w->rect().contains(me->pos())) {
                        onClicked();
                    }
                }
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// DataHubWidget
// ---------------------------------------------------------------------------

DataHubWidget::DataHubWidget(QWidget *parent) : QWidget(parent)
{
    buildUi();
}

// ── buildUi ──────────────────────────────────────────────────────────────────

void DataHubWidget::buildUi()
{
    // Root layout — no scroll; the hub fits a standard desktop window.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Wrap content in a scroll area so it degrades gracefully on small windows.
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(scrollArea);

    auto *content = new QWidget(scrollArea);
    scrollArea->setWidget(content);
    auto *vbox = new QVBoxLayout(content);
    vbox->setContentsMargins(32, 28, 32, 28);
    vbox->setSpacing(20);

    // ── 1. Header ─────────────────────────────────────────────────────────────
    {
        // Main title — Lato Black, brand blue. Lato Black gives the editorial
        // weight called for in the VisIVO brand guidelines for major headings.
        auto *titleLabel = new QLabel(u"VisIVO Data Hub"_s, content);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(28);
            f.setWeight(QFont::Black);
            f.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
            titleLabel->setFont(f);
        }
        titleLabel->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kPrimary)));
        vbox->addWidget(titleLabel);

        // Subtitle — Lato Regular, secondary text colour.
        auto *sub = new QLabel(
                u"VisIVO uses a backend-authoritative workflow. "
                "Scientific operations are available for backend-visible datasets."_s,
                content);
        sub->setWordWrap(true);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(13);
            f.setWeight(QFont::Normal);
            sub->setFont(f);
        }
        sub->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant)));
        vbox->addWidget(sub);
    }

    // Separator
    {
        auto *sep = new QFrame(content);
        sep->setFixedHeight(1);
        sep->setStyleSheet(
                QStringLiteral("background-color: rgba(4, 138, 191, 0.22); border: none;"));
        vbox->addWidget(sep);
    }

    // ── 2. Operational Backend card ───────────────────────────────────────────
    vbox->addWidget(buildBackendCard());

    // ── 3. Primary Operations section label + 4-card grid ────────────────────
    vbox->addWidget(VisivoTheme::makeSectionLabel(u"Primary Operations"_s, content));
    vbox->addWidget(buildActionGrid());

    // ── 4. Lower row: Recent Datasets + System Insights ───────────────────────
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(16);
        row->addWidget(buildRecentDatasetsPanel(), 2);
        row->addWidget(buildSystemInsightsPanel(), 1);
        vbox->addLayout(row);
    }

    // ── 5. Footer ─────────────────────────────────────────────────────────────
    {
        auto *sep = new QFrame(content);
        sep->setFixedHeight(1);
        sep->setStyleSheet(
                QStringLiteral("background-color: rgba(4, 138, 191, 0.15); border: none;"));
        vbox->addWidget(sep);

        auto *footer = new QLabel(
                u"VisIVO uses a backend-authoritative workflow. "
                "Scientific operations are computed on remote backend nodes to ensure "
                "data integrity and high-performance processing."_s,
                content);
        footer->setWordWrap(true);
        footer->setStyleSheet(
                QStringLiteral("color: rgba(145, 170, 235, 0.55); font-size: 11px; "
                                "background: transparent;"));
        vbox->addWidget(footer);
    }

    vbox->addStretch(1);
}

// ── buildBackendCard ─────────────────────────────────────────────────────────

QFrame *DataHubWidget::buildBackendCard()
{
    // Wide banner: gradient feel via a slightly brighter surface-container-high.
    auto *card = new QFrame(this);
    card->setStyleSheet(
            QStringLiteral(
                    "QFrame#backendCard {"
                    "  background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
                    "    stop:0 %1, stop:1 %2);"
                    "  border-radius: 12px;"
                    "  border: 1px solid rgba(4, 138, 191, 0.22);"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow),
                         QLatin1StringView(VisivoTheme::kSurfaceContainerHigh)));
    card->setObjectName(u"backendCard"_s);

    auto *hbox = new QHBoxLayout(card);
    hbox->setContentsMargins(24, 20, 24, 20);
    hbox->setSpacing(20);

    // Left: info stack
    auto *infoCol = new QVBoxLayout;
    infoCol->setSpacing(8);

    // "OPERATIONAL BACKEND" label + badge row
    {
        auto *topRow = new QHBoxLayout;
        topRow->setSpacing(10);
        topRow->setContentsMargins(0, 0, 0, 0);

        auto *sectionLbl = VisivoTheme::makeSectionLabel(u"Operational Backend"_s, card);
        topRow->addWidget(sectionLbl);

        this->labelStatusBadge = VisivoTheme::makeBadge(
                u"● Status unknown"_s, VisivoTheme::BadgeKind::Neutral, card);
        topRow->addWidget(this->labelStatusBadge);
        topRow->addStretch(1);
        infoCol->addLayout(topRow);
    }

    // Backend URL — large, prominent. Lato Bold matches brand heading weight.
    this->labelBackendUrl = new QLabel(u"(not configured)"_s, card);
    {
        QFont f(QStringLiteral("Lato"));
        f.setPixelSize(18);
        f.setWeight(QFont::Bold);
        this->labelBackendUrl->setFont(f);
        this->labelBackendUrl->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface)));
        this->labelBackendUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        this->labelBackendUrl->setCursor(Qt::IBeamCursor);
        this->labelBackendUrl->setWordWrap(true);
        infoCol->addWidget(this->labelBackendUrl);
    }

    // Detail / error text
    this->labelStatusDetail = new QLabel(u"Set the backend URL in Edit → Settings."_s, card);
    this->labelStatusDetail->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant)));
    this->labelStatusDetail->setWordWrap(true);
    infoCol->addWidget(this->labelStatusDetail);

    hbox->addLayout(infoCol, 1);

    // Right: button
    this->btnCheckConnection = VisivoTheme::makePrimaryButton(u"Check Connection"_s, card);
    this->btnCheckConnection->setMinimumWidth(170);
    hbox->addWidget(this->btnCheckConnection, 0, Qt::AlignVCenter);

    QObject::connect(this->btnCheckConnection, &QPushButton::clicked,
                     this, &DataHubWidget::refreshStatus);

    return card;
}

// ── buildActionGrid ───────────────────────────────────────────────────────────

QFrame *DataHubWidget::buildActionGrid()
{
    auto *container = new QFrame(this);
    container->setStyleSheet(u"QFrame { background: transparent; border: none; }"_s);
    auto *grid = new QGridLayout(container);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(12);

    struct CardInfo {
        QString symbol;
        QString title;
        QString desc;
    };
    const CardInfo cards[] = {
        { u"↓"_s,  u"Open Remote Dataset"_s,    u"Browse and open FITS images or spectral cubes via the backend."_s },
        { u"◈"_s,  u"Open 3D Catalogue"_s,      u"Visualise astronomical CSV catalogues in the 3D source viewer."_s },
        { u"⊞"_s,  u"Open VBT"_s,               u"Load a VisIVO Binary Table for point-cloud or volume rendering."_s },
        { u"✦"_s,  u"HiPS Viewer"_s,             u"Explore hierarchical progressive sky surveys (coming soon)."_s },
    };

    // Connect each card to the matching signal.
    void (DataHubWidget::*actions[])() = {
        &DataHubWidget::openDataRequested,
        &DataHubWidget::openCatalogueRequested,
        &DataHubWidget::openVbtRequested,
        &DataHubWidget::openHiPSRequested,
    };

    for (int i = 0; i < 4; ++i) {
        auto *card = makeActionCard(cards[i].symbol, cards[i].title, cards[i].desc, container);
        auto action = actions[i];
        auto *filter = new CardClickFilter([this, action]() { emit (this->*action)(); }, card);
        card->installEventFilter(filter);
        grid->addWidget(card, 0, i);
        grid->setColumnStretch(i, 1);
    }

    return container;
}

// ── makeActionCard ────────────────────────────────────────────────────────────

QFrame *DataHubWidget::makeActionCard(const QString &symbol, const QString &title,
                                      const QString &desc, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setAttribute(Qt::WA_Hover);
    card->setCursor(Qt::PointingHandCursor);
    // Normal + hover states in a single stylesheet using the :hover pseudo-class.
    // WA_Hover must be set for :hover to fire on QFrame.
    card->setStyleSheet(
            QStringLiteral(
                    "QFrame {"
                    "  background-color: %1;"
                    "  border-radius: 10px;"
                    "  border: 1px solid rgba(4, 138, 191, 0.14);"
                    "}"
                    "QFrame:hover {"
                    "  background-color: %2;"
                    "  border-color: rgba(4, 138, 191, 0.38);"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHigh),
                         QLatin1StringView(VisivoTheme::kSurfaceBright)));

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 20, 20, 20);
    vbox->setSpacing(8);

    // Icon symbol — large, accent-coloured
    auto *iconLbl = new QLabel(symbol, card);
    {
        QFont f = iconLbl->font();
        f.setPointSize(f.pointSize() + 8);
        iconLbl->setFont(f);
        iconLbl->setStyleSheet(
                QStringLiteral("color: %1; background: transparent; border: none;")
                        .arg(QLatin1StringView(VisivoTheme::kPrimary)));
        iconLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    vbox->addWidget(iconLbl);

    // Title — Lato Bold (brand heading weight for action card titles)
    auto *titleLbl = new QLabel(title, card);
    {
        QFont f(QStringLiteral("Lato"));
        f.setPixelSize(15);
        f.setWeight(QFont::Bold);
        titleLbl->setFont(f);
        titleLbl->setStyleSheet(
                QStringLiteral("color: %1; background: transparent; border: none;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface)));
        titleLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    vbox->addWidget(titleLbl);

    // Description — small, muted
    auto *descLbl = new QLabel(desc, card);
    descLbl->setWordWrap(true);
    descLbl->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; background: transparent; border: none;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant)));
    descLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
    vbox->addWidget(descLbl);

    vbox->addStretch(1);
    return card;
}

// ── buildRecentDatasetsPanel ──────────────────────────────────────────────────

QFrame *DataHubWidget::buildRecentDatasetsPanel()
{
    auto *card = VisivoTheme::makeCard(this);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->addWidget(VisivoTheme::makeSectionLabel(u"Recent Datasets"_s, card));
    header->addStretch(1);
    vbox->addLayout(header);

    this->listRecentDatasets = new QListWidget(card);
    this->listRecentDatasets->setFrameShape(QFrame::NoFrame);
    this->listRecentDatasets->setSelectionMode(QAbstractItemView::NoSelection);
    this->listRecentDatasets->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->listRecentDatasets->setMinimumHeight(120);

    // Empty-state placeholder
    auto *emptyItem = new QListWidgetItem(
            u"No recently opened datasets.\n"
            "Open a dataset using the actions above."_s,
            this->listRecentDatasets);
    emptyItem->setFlags(Qt::NoItemFlags);
    emptyItem->setForeground(QColor(VisivoTheme::kOnSurfaceVariant));

    vbox->addWidget(this->listRecentDatasets, 1);
    return card;
}

// ── buildSystemInsightsPanel ──────────────────────────────────────────────────

QFrame *DataHubWidget::buildSystemInsightsPanel()
{
    auto *card = VisivoTheme::makeCard(this, true /* elevated */);
    card->setMinimumWidth(200);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(16);

    vbox->addWidget(VisivoTheme::makeSectionLabel(u"System Insights"_s, card));

    // Worker count
    {
        auto *metricBox = new QVBoxLayout;
        auto *lbl = new QLabel(u"Active Workers"_s, card);
        lbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; letter-spacing: 0.1em; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kPrimaryDim)));
        metricBox->addWidget(lbl);
        this->labelWorkers = new QLabel(u"—"_s, card);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(32);
            f.setWeight(QFont::Black);
            this->labelWorkers->setFont(f);
            this->labelWorkers->setStyleSheet(
                    QStringLiteral("color: %1; background: transparent;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurface)));
        }
        metricBox->addWidget(this->labelWorkers);
        vbox->addLayout(metricBox);
    }

    // Session count
    {
        auto *metricBox = new QVBoxLayout;
        auto *lbl = new QLabel(u"Active Sessions"_s, card);
        lbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; letter-spacing: 0.1em; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kPrimaryDim)));
        metricBox->addWidget(lbl);
        this->labelSessions = new QLabel(u"—"_s, card);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(32);
            f.setWeight(QFont::Black);
            this->labelSessions->setFont(f);
            this->labelSessions->setStyleSheet(
                    QStringLiteral("color: %1; background: transparent;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurface)));
        }
        metricBox->addWidget(this->labelSessions);
        vbox->addLayout(metricBox);
    }

    // Task registry
    {
        auto *metricBox = new QVBoxLayout;
        auto *lbl = new QLabel(u"Task Registry"_s, card);
        lbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; letter-spacing: 0.1em; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kPrimaryDim)));
        metricBox->addWidget(lbl);
        this->labelTaskRegistry = new QLabel(u"—"_s, card);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(32);
            f.setWeight(QFont::Black);
            this->labelTaskRegistry->setFont(f);
            this->labelTaskRegistry->setStyleSheet(
                    QStringLiteral("color: %1; background: transparent;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurface)));
        }
        metricBox->addWidget(this->labelTaskRegistry);
        vbox->addLayout(metricBox);
    }

    // Separator + quote
    {
        auto *sep = new QFrame(card);
        sep->setFixedHeight(1);
        sep->setStyleSheet(
                QStringLiteral("background-color: rgba(4, 138, 191, 0.18); border: none;"));
        vbox->addWidget(sep);

        auto *quote = new QLabel(
                u"Scientific operations are computed on remote backend nodes to ensure "
                "data integrity and high-performance processing."_s,
                card);
        quote->setWordWrap(true);
        quote->setStyleSheet(
                QStringLiteral("color: rgba(145, 170, 235, 0.6); font-size: 11px; "
                                "font-style: italic; background: transparent;"));
        vbox->addWidget(quote);
    }

    vbox->addStretch(1);
    return card;
}

// ── setBackendUrl ─────────────────────────────────────────────────────────────

void DataHubWidget::setBackendUrl(const QString &url, const QString &token)
{
    this->backendUrl   = url;
    this->backendToken = token;

    if (this->labelBackendUrl) {
        this->labelBackendUrl->setText(url.isEmpty() ? u"(not configured)"_s : url);
    }
    if (this->labelStatusBadge) {
        this->labelStatusBadge->setText(u"● Status unknown"_s);
        this->labelStatusBadge->setStyleSheet(
                QStringLiteral(
                        "QLabel {"
                        "  background-color: %1;"
                        "  color: %2;"
                        "  font-size: 10px; font-weight: 700;"
                        "  letter-spacing: 0.12em;"
                        "  padding: 3px 10px;"
                        "  border-radius: 10px; border: none;"
                        "}")
                        .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHighest),
                             QLatin1StringView(VisivoTheme::kOnSurfaceVariant)));
    }
    if (this->labelStatusDetail) {
        this->labelStatusDetail->setText(u"Click \"Check Connection\" to verify backend reachability."_s);
    }
}

// ── refreshStatus ─────────────────────────────────────────────────────────────

void DataHubWidget::refreshStatus()
{
    if (this->backendUrl.isEmpty()) {
        if (this->labelStatusBadge) {
            this->labelStatusBadge->setText(u"● Not configured"_s);
            this->labelStatusBadge->setStyleSheet(
                    QStringLiteral(
                            "QLabel {"
                            "  background-color: %1; color: %2;"
                            "  font-size: 10px; font-weight: 700;"
                            "  letter-spacing: 0.12em; padding: 3px 10px;"
                            "  border-radius: 10px; border: none;"
                            "}")
                            .arg(QLatin1StringView(VisivoTheme::kWarningContainer),
                                 QLatin1StringView(VisivoTheme::kOnWarningContainer)));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    u"No backend URL configured. Set it in Edit → Settings."_s);
        }
        return;
    }

    if (this->btnCheckConnection) {
        this->btnCheckConnection->setEnabled(false);
        this->btnCheckConnection->setText(u"Checking…"_s);
    }
    if (this->labelStatusDetail) {
        this->labelStatusDetail->setText(u"Contacting backend…"_s);
    }

    BackendClient client(this->backendUrl, this->backendToken);
    const BackendHealthResult health = client.health();

    if (health.ok) {
        if (this->labelStatusBadge) {
            this->labelStatusBadge->setText(u"● Connected"_s);
            this->labelStatusBadge->setStyleSheet(
                    QStringLiteral(
                            "QLabel {"
                            "  background-color: %1; color: %2;"
                            "  font-size: 10px; font-weight: 700;"
                            "  letter-spacing: 0.12em; padding: 3px 10px;"
                            "  border-radius: 10px; border: none;"
                            "}")
                            .arg(QLatin1StringView(VisivoTheme::kTertiaryContainer),
                                 QLatin1StringView(VisivoTheme::kOnTertiaryFixed)));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    QStringLiteral("Backend reachable — workers: %1, sessions: %2")
                            .arg(health.workers)
                            .arg(health.activeSessions));
        }
        if (this->labelWorkers) {
            this->labelWorkers->setText(
                    health.workers > 0 ? QString::number(health.workers) : u"—"_s);
        }
        if (this->labelSessions) {
            this->labelSessions->setText(
                    health.activeSessions > 0 ? QString::number(health.activeSessions) : u"—"_s);
        }
        if (this->labelTaskRegistry) {
            this->labelTaskRegistry->setText(
                    health.taskRegistryEntries > 0
                            ? QString::number(health.taskRegistryEntries)
                            : u"—"_s);
        }
        qDebug().noquote()
                << QStringLiteral("[DataHub] backend reachable — workers=%1 sessions=%2 tasks=%3")
                           .arg(health.workers)
                           .arg(health.activeSessions)
                           .arg(health.taskRegistryEntries);
    } else {
        if (this->labelStatusBadge) {
            this->labelStatusBadge->setText(u"● Unreachable"_s);
            this->labelStatusBadge->setStyleSheet(
                    QStringLiteral(
                            "QLabel {"
                            "  background-color: %1; color: %2;"
                            "  font-size: 10px; font-weight: 700;"
                            "  letter-spacing: 0.12em; padding: 3px 10px;"
                            "  border-radius: 10px; border: none;"
                            "}")
                            .arg(QLatin1StringView(VisivoTheme::kErrorContainer),
                                 QLatin1StringView(VisivoTheme::kOnErrorContainer)));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    health.error.isEmpty()
                            ? u"No response from backend."_s
                            : health.error);
        }
        // Reset metrics to unknown
        for (auto *lbl : { this->labelWorkers.data(),
                           this->labelSessions.data(),
                           this->labelTaskRegistry.data() }) {
            if (lbl) lbl->setText(u"—"_s);
        }
        qDebug().noquote()
                << QStringLiteral("[DataHub] backend unreachable — %1").arg(health.error);
    }

    if (this->btnCheckConnection) {
        this->btnCheckConnection->setEnabled(true);
        this->btnCheckConnection->setText(u"Check Connection"_s);
    }
}
