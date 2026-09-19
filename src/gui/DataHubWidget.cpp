#include "DataHubWidget.h"
#include "CardClickFilter.h"
#include "app/BackendClient.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTimer>
#include <QtConcurrentRun>

#include <cmath>

using namespace Qt::StringLiterals;

// ── DataHubWidget: core layout / cards (CardClickFilter in CardClickFilter.h) ──

namespace {

QString actionCardSvg(const QString &kind, const QString &stroke)
{
    if (kind == u"dataset"_s) {
        return QString(
                       "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' fill='none'>"
                       "<path d='M10 14h18l4 4h6v16a4 4 0 0 1-4 4H10a4 4 0 0 1-4-4V18a4 4 0 0 1 4-4Z' "
                       "stroke='%1' stroke-width='2.6' stroke-linejoin='round'/>"
                       "<path d='M24 23v10M19 28h10' stroke='%1' stroke-width='2.6' stroke-linecap='round'/>"
                       "</svg>")
                .arg(stroke);
    }
    if (kind == u"catalogue"_s) {
        return QString(
                       "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' fill='none'>"
                       "<circle cx='14' cy='15' r='2.8' fill='%1'/>"
                       "<circle cx='32' cy='12' r='2.8' fill='%1'/>"
                       "<circle cx='24' cy='24' r='2.8' fill='%1'/>"
                       "<circle cx='12' cy='32' r='2.8' fill='%1'/>"
                       "<circle cx='34' cy='31' r='2.8' fill='%1'/>"
                       "<path d='M14 15 24 24 32 12M24 24 12 32M24 24 34 31' stroke='%1' stroke-width='2.4' stroke-linecap='round'/>"
                       "</svg>")
                .arg(stroke);
    }
    if (kind == u"vbt"_s) {
        return QString(
                       "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' fill='none'>"
                       "<rect x='10' y='10' width='10' height='10' stroke='%1' stroke-width='2.4'/>"
                       "<rect x='28' y='10' width='10' height='10' stroke='%1' stroke-width='2.4'/>"
                       "<rect x='10' y='28' width='10' height='10' stroke='%1' stroke-width='2.4'/>"
                       "<rect x='28' y='28' width='10' height='10' stroke='%1' stroke-width='2.4'/>"
                       "<path d='M20 15h8M15 20v8M33 20v8M20 33h8' stroke='%1' stroke-width='2.2' stroke-linecap='round'/>"
                       "</svg>")
                .arg(stroke);
    }
    return QString(
                   "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48' fill='none'>"
                   "<circle cx='24' cy='24' r='13' stroke='%1' stroke-width='2.4'/>"
                   "<path d='M11 24h26M24 11c4 4 6 8 6 13s-2 9-6 13c-4-4-6-8-6-13s2-9 6-13Z' stroke='%1' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'/>"
                   "<circle cx='24' cy='24' r='2.6' fill='%1'/>"
                   "</svg>")
            .arg(stroke);
}

QPixmap actionCardIconPixmap(const QString &kind, const QString &stroke, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QSvgRenderer renderer(actionCardSvg(kind, stroke).toUtf8());
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pixmap;
}

QPixmap visivoBrandIconPixmap(const QString &stroke, int width, int height)
{
    QPixmap pixmap(width, height);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor color(stroke);
    QPen pen(color);
    pen.setWidthF(2.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QRectF r(6.0, 6.0, width - 12.0, height - 12.0);

    painter.save();
    painter.translate(r.center());
    painter.rotate(-22.0);
    painter.translate(-r.center());
    painter.drawEllipse(r);
    painter.restore();

    painter.save();
    painter.translate(r.center());
    painter.rotate(26.0);
    painter.translate(-r.center());
    painter.drawArc(r.adjusted(5.0, 10.0, -5.0, -10.0), 25 * 16, 220 * 16);
    painter.restore();

    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(width * 0.76, height * 0.30), 4.8, 4.8);
    painter.drawEllipse(QPointF(width * 0.28, height * 0.66), 3.0, 3.0);
    painter.drawEllipse(QPointF(width * 0.77, height * 0.73), 7.0, 7.0);

    return pixmap;
}

}  // namespace

DataHubWidget::DataHubWidget(QWidget *parent) : QWidget(parent)
{
    buildUi();
    // Automatic backend health check at startup.
    QTimer::singleShot(0, this, &DataHubWidget::refreshStatus);

    // Periodic refresh of System Insights so the running counter and recent
    // operations trail stay current without a manual click. 4 s is a balance
    // between responsiveness for short tasks (moment maps run in seconds) and
    // not flooding the backend with health pings.
    this->insightsTimer = new QTimer(this);
    this->insightsTimer->setInterval(4000);
    QObject::connect(this->insightsTimer, &QTimer::timeout,
                     this, &DataHubWidget::refreshStatus);
    this->insightsTimer->start();

    // Theme change → full rebuild of the hub UI. Most of our widgets bake
    // theme hex values into inline stylesheets at construction time, so a
    // QSS-only refresh would only re-tint the global rules and leave the
    // hub's cards/labels stranded in the previous theme. A rebuild is the
    // pragmatic way to get every label, chip, and pill back in sync.
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                     this, [this](EffectiveTheme) { this->rebuildOnThemeChange(); });
}

void DataHubWidget::rebuildOnThemeChange()
{
    // Clear every QPointer that buildUi populates so the helper methods
    // don't reach for dangling labels mid-rebuild. Backend URL/token and
    // the RecentDatasetsManager pointer are user state and are preserved.
    this->labelBackendUrl.clear();
    this->labelStatusBadge.clear();
    this->labelStatusDetail.clear();
    this->btnCheckConnection.clear();

    this->labelRunningTasks.clear();
    this->labelRunningCaption.clear();
    this->labelRecentHeader.clear();
    this->recentTasksContainer.clear();
    this->labelRecentEmpty.clear();
    this->labelCapacity.clear();

    this->statusBarHealth.clear();
    this->statusBarWorkers.clear();
    this->statusBarSession.clear();
    this->statusBarSanity.clear();

    this->recentItemsLayout.clear();

    // Rip out the old layout (and all of its child widgets) by parenting it
    // to a throwaway QWidget that goes out of scope here. Standard Qt idiom
    // for replacing a widget's layout — `delete this->layout()` alone leaves
    // the children orphaned in the parent.
    if (auto *old = this->layout()) {
        QWidget temp;
        temp.setLayout(old);
    }

    this->buildUi();

    // Repopulate live data after the new tree is in place.
    if (!this->backendUrl.isEmpty()) {
        QTimer::singleShot(0, this, &DataHubWidget::refreshStatus);
    }
    this->refreshRecentDatasets();
}

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
        // [ VisIVO orbital logo ]  [ title + subtitle ]
        // Logo sits at 44 px height — large enough to read the wordmark clearly
        // without overwhelming the header. No glow or color effects: asset as-is.
        auto *headerRow = new QHBoxLayout;
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(16);

        if (auto *logo = VisivoTheme::createVisivoLogoLabel(44, content)) {
            headerRow->addWidget(logo, 0, Qt::AlignVCenter);
        }

        // Title + subtitle stacked column
        auto *titleCol = new QVBoxLayout;
        titleCol->setContentsMargins(0, 0, 0, 0);
        titleCol->setSpacing(4);

        // Main title — Lato Black, brand blue.
        auto *titleLabel = new QLabel(u"Data Hub"_s, content);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(26);
            f.setWeight(QFont::Black);
            titleLabel->setFont(f);
        }
        titleLabel->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kPrimary())));
        titleCol->addWidget(titleLabel);

        // Subtitle — Lato Regular, muted.
        auto *sub = new QLabel(
                u"Backend-authoritative workflow, scientific operations run on remote nodes."_s,
                content);
        sub->setWordWrap(true);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(12);
            f.setWeight(QFont::Normal);
            sub->setFont(f);
        }
        sub->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        titleCol->addWidget(sub);

        headerRow->addLayout(titleCol, 1);
        vbox->addLayout(headerRow);
    }

    // Separator
    {
        auto *sep = new QFrame(content);
        sep->setFixedHeight(1);
        sep->setStyleSheet(
                QStringLiteral("background-color: rgba(4, 138, 191, 0.22); border: none;"));
        vbox->addWidget(sep);
    }

    // ── 2. Global search bar (opens ⌘K command palette) ──────────────────────
    vbox->addWidget(buildSearchBar());

    // ── 3. Backend toolbar (path-pill + connection status) ───────────────────
    vbox->addWidget(buildBackendCard());

    // ── 4. Mission Control body: 60/40 two-column layout ─────────────────────
    //    LEFT (60%): Recent Datasets — the user's main entry into work
    //    RIGHT (40%): Quick actions stack + Last 5 jobs feed
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(16);

        row->addWidget(buildRecentDatasetsPanel(), 60);

        auto *rightCol = new QVBoxLayout;
        rightCol->setSpacing(16);
        rightCol->addWidget(buildQuickActionsList());
        rightCol->addWidget(buildLast5JobsPanel(), 1);
        row->addLayout(rightCol, 40);

        vbox->addLayout(row, 1);
    }

    // ── 4b. Workspace Exports (persistent FITS artefacts) ────────────────────
    // Full-width panel below the main row: shows everything currently in
    // the backend's Workspace Exports dir with per-row buttons (open in
    // VisIVO, download to local FS, delete). Refresh happens on first
    // show + on demand via the panel's own refresh button.
    vbox->addWidget(buildWorkspaceExportsPanel());

    // ── 5. Credits line (small, right-aligned) ───────────────────────────────
    {
        auto *credits = new QLabel(
                u"VisIVO Visual Analytics, crafted with ❤ by VisIVOLab team"_s,
                content);
        credits->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; "
                                "background: transparent; padding: 4px 0 0 0;")
                        .arg(QLatin1StringView(VisivoTheme::kSecondary())));
        credits->setAlignment(Qt::AlignCenter);
        vbox->addWidget(credits);
    }

    // ── 6. Status bar (Layout B: bottom-anchored, dense, inspector-grade) ────
    //    Health + activity + session + sanity badge in a single thin row.
    vbox->addWidget(buildStatusBar());
}

QFrame *DataHubWidget::buildBackendCard()
{
    // Layout B's `.va-toolbar`: a slim horizontal bar at the top of a screen,
    // anchored by a path-pill that shows the active context (here: the backend
    // endpoint URL). Status badge + workers tag + Check Connection sit to the
    // right. Replaces the previous large "Operational Backend" card.
    auto *bar = new QFrame(this);
    bar->setObjectName(u"backendCard"_s);
    bar->setStyleSheet(
            QStringLiteral(
                    "QFrame#backendCard {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 8px;"
                    "}")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                         QLatin1StringView(VisivoTheme::kOutline())));

    auto *hbox = new QHBoxLayout(bar);
    hbox->setContentsMargins(12, 8, 12, 8);
    hbox->setSpacing(10);

    // ── Path-pill: home icon + backend URL (selectable). The home glyph sets
    //    the visual rhythm; the URL itself is mono-spaced and selectable so
    //    users can copy it.
    auto *pill = new QFrame(bar);
    pill->setObjectName(u"pathPill"_s);
    pill->setStyleSheet(
            QStringLiteral(
                    "QFrame#pathPill {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 6px;"
                    "}"
                    "QLabel { background: transparent; }")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHigh()),
                         QLatin1StringView(VisivoTheme::kOutline())));
    auto *pillLayout = new QHBoxLayout(pill);
    pillLayout->setContentsMargins(10, 4, 10, 4);
    pillLayout->setSpacing(8);

    auto *pillIcon = new QLabel(u"⌂"_s, pill);
    pillIcon->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px;")
                    .arg(QLatin1StringView(VisivoTheme::kPrimary())));
    pillLayout->addWidget(pillIcon);

    this->labelBackendUrl = new QLabel(u"(not configured)"_s, pill);
    {
        QFont f(QStringLiteral("JetBrains Mono"));
        f.setStyleHint(QFont::Monospace);
        f.setPixelSize(12);
        this->labelBackendUrl->setFont(f);
    }
    this->labelBackendUrl->setStyleSheet(
            QStringLiteral("color: %1;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
    this->labelBackendUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->labelBackendUrl->setCursor(Qt::IBeamCursor);
    pillLayout->addWidget(this->labelBackendUrl);

    hbox->addWidget(pill);

    // ── Status badge (Connected / Unreachable / unknown) ─────────────────────
    this->labelStatusBadge = VisivoTheme::makeBadge(
            u"● Status unknown"_s, VisivoTheme::BadgeKind::Neutral, bar);
    hbox->addWidget(this->labelStatusBadge);

    // ── Detail caption (sits inline, replacing the old separate detail row).
    //    Visible only when the badge state needs it (errors, hints).
    this->labelStatusDetail = new QLabel(
            u"Set the backend URL in Edit → Settings."_s, bar);
    this->labelStatusDetail->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    hbox->addWidget(this->labelStatusDetail, 1);

    // ── Check Connection button (right edge) ────────────────────────────────
    this->btnCheckConnection = VisivoTheme::makeSecondaryButton(
            u"Check Connection"_s, bar);
    QObject::connect(this->btnCheckConnection, &QPushButton::clicked,
                     this, &DataHubWidget::refreshStatus);
    hbox->addWidget(this->btnCheckConnection, 0, Qt::AlignVCenter);

    return bar;
}

QFrame *DataHubWidget::buildSearchBar()
{
    // Wide search field that doubles as the command palette trigger. We don't
    // implement live-filtering here — clicking anywhere on the bar opens the
    // shared CommandPalette which already covers filesystem + recents +
    // VLKB-eventually + actions.
    auto *bar = new QFrame(this);
    bar->setObjectName(u"hubSearchBar"_s);
    bar->setCursor(Qt::PointingHandCursor);
    bar->setStyleSheet(
            QStringLiteral(
                    "QFrame#hubSearchBar {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 8px;"
                    "}"
                    "QFrame#hubSearchBar:hover {"
                    "  border-color: %3;"
                    "}"
                    "QFrame#hubSearchBar QLabel { background: transparent; }")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHigh()),
                         QLatin1StringView(VisivoTheme::kOutline()),
                         QLatin1StringView(VisivoTheme::kPrimary())));

    auto *hbox = new QHBoxLayout(bar);
    hbox->setContentsMargins(14, 9, 14, 9);
    hbox->setSpacing(10);

    auto *icon = new QLabel(u"⌕"_s, bar);
    icon->setStyleSheet(
            QStringLiteral("color: %1; font-size: 16px;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    hbox->addWidget(icon);

    auto *placeholder = new QLabel(
            u"Search FITS files, catalogues, recent sessions, actions…"_s, bar);
    placeholder->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    hbox->addWidget(placeholder, 1);

    // Scope chips — read-only hints showing what the palette covers.
    auto makeScope = [&](const QString &t) {
        auto *l = new QLabel(t, bar);
        l->setStyleSheet(
                QStringLiteral(
                        "background: %1; color: %2;"
                        "font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                        "font-size: 10px; padding: 2px 6px; border-radius: 3px;"
                        "border: 1px solid %3;")
                        .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                             QLatin1StringView(VisivoTheme::kSecondary()),
                             QLatin1StringView(VisivoTheme::kOutline())));
        return l;
    };
    hbox->addWidget(makeScope(u"recents"_s));
    hbox->addWidget(makeScope(u"actions"_s));

    auto *kbd = new QLabel(u"⌘ K"_s, bar);
    kbd->setStyleSheet(
            QStringLiteral(
                    "background: %1; color: %2;"
                    "font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "font-size: 11px; padding: 3px 8px; border-radius: 4px;"
                    "border: 1px solid %3;")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                         QLatin1StringView(VisivoTheme::kOnSurface()),
                         QLatin1StringView(VisivoTheme::kOutline())));
    hbox->addWidget(kbd);

    // Click anywhere on the bar = open palette.
    bar->installEventFilter(this);

    return bar;
}

QFrame *DataHubWidget::buildQuickActionsList()
{
    // Replaces the 4-big-card "Primary Operations" grid. Each row is a
    // single-line action with icon + label + subtitle + optional shortcut hint
    // — denser, less landing-page-y, more "tool on a desk".
    auto *card = VisivoTheme::makeCard(this, /*elevated=*/true);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(8);

    vbox->addWidget(VisivoTheme::makeSectionLabel(u"Quick actions"_s, card));

    struct Spec {
        QString iconKind;    // matches existing actionCardSvg() vocabulary
        QString title;
        QString subtitle;
        QString shortcut;
        const char *signalName;  // we connect via switch
        QString section;     // non-empty = start a new group with this heading
    };
    // One entry for files, one group for archives. There used to be three file
    // entries — dataset / catalogue / VBT — which asked the user to classify
    // the file before opening it; two of them accepted `.fits`, so the question
    // was often unanswerable. The backend classifies it now (/v1/files/classify)
    // and only asks when the content is genuinely ambiguous.
    const std::array<Spec, 3> specs{
            Spec{u"dataset"_s,   u"Open…"_s,
                 u"FITS image or cube, catalogue, VBT — identified on open"_s, u"⌘O"_s,
                 "openAnythingRequested", QString()},
            // Sky-map engines: both open as their own viewer tabs.
            Spec{u"hips"_s,      u"HiPS Viewer"_s,
                 u"Hierarchical sky surveys (DSS, 2MASS, …)"_s, QString(),
                 "openHiPSRequested", u"Archives"_s},
            Spec{u"hips"_s,      u"VLKB Inventory"_s,
                 u"Browse VLKB sources & add as image layers"_s, QString(),
                 "openVLKBRequested", QString()},
    };

    for (const auto &s : specs) {
        if (!s.section.isEmpty()) {
            vbox->addSpacing(6);
            vbox->addWidget(VisivoTheme::makeSectionLabel(s.section, card));
        }
        auto *row = new QFrame(card);
        row->setObjectName(u"qaRow"_s);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(
                QStringLiteral(
                        "QFrame#qaRow {"
                        "  background: transparent;"
                        "  border: 1px solid transparent;"
                        "  border-radius: 6px;"
                        "}"
                        "QFrame#qaRow:hover {"
                        "  background-color: %1;"
                        "  border-color: %2;"
                        "}"
                        "QFrame#qaRow QLabel { background: transparent; }")
                        .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHigh()),
                             QLatin1StringView(VisivoTheme::kOutline())));

        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(10, 8, 10, 8);
        rl->setSpacing(12);

        auto *icon = new QLabel(row);
        icon->setPixmap(actionCardIconPixmap(
                s.iconKind,
                QString::fromLatin1(VisivoTheme::kPrimaryDim()), 22));
        icon->setFixedSize(28, 28);
        icon->setAlignment(Qt::AlignCenter);
        rl->addWidget(icon);

        auto *labelCol = new QVBoxLayout;
        labelCol->setContentsMargins(0, 0, 0, 0);
        labelCol->setSpacing(0);
        auto *title = new QLabel(s.title, row);
        title->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; font-weight: 600;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        labelCol->addWidget(title);
        auto *sub = new QLabel(s.subtitle, row);
        sub->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        labelCol->addWidget(sub);
        rl->addLayout(labelCol, 1);

        if (!s.shortcut.isEmpty()) {
            auto *kbd = new QLabel(s.shortcut, row);
            kbd->setStyleSheet(
                    QStringLiteral(
                            "background: %1; color: %2;"
                            "font-family: 'JetBrains Mono', 'SF Mono', monospace;"
                            "font-size: 10px; padding: 2px 6px; border-radius: 3px;"
                            "border: 1px solid %3;")
                            .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                                 QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                                 QLatin1StringView(VisivoTheme::kOutline())));
            rl->addWidget(kbd);
        }

        // Wire click via property so the eventFilter can dispatch.
        row->setProperty("qaSignal", QString::fromLatin1(s.signalName));
        row->installEventFilter(this);

        vbox->addWidget(row);
    }

    vbox->addStretch(1);
    return card;
}

QFrame *DataHubWidget::buildLast5JobsPanel()
{
    auto *card = VisivoTheme::makeCard(this, /*elevated=*/true);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(10);

    auto *hdrRow = new QHBoxLayout;
    hdrRow->setContentsMargins(0, 0, 0, 0);
    hdrRow->addWidget(VisivoTheme::makeSectionLabel(u"Last 5 jobs"_s, card));
    hdrRow->addStretch(1);
    this->labelRunningCaption = new QLabel(u"Idle"_s, card);
    this->labelRunningCaption->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    hdrRow->addWidget(this->labelRunningCaption);
    vbox->addLayout(hdrRow);

    // Reuse the existing recent-tasks rendering machinery: same container +
    // empty-state label + header that updateSystemInsights() already updates.
    this->labelRecentHeader = new QLabel(u""_s, card);
    this->labelRecentHeader->setVisible(false);  // header is now the section label

    this->recentTasksContainer = new QWidget(card);
    this->recentTasksContainer->setStyleSheet(u"background: transparent;"_s);
    auto *recentLayout = new QVBoxLayout(this->recentTasksContainer);
    recentLayout->setContentsMargins(0, 0, 0, 0);
    recentLayout->setSpacing(6);
    vbox->addWidget(this->recentTasksContainer);

    this->labelRecentEmpty = new QLabel(
            u"No jobs yet. Open a dataset and run a tool to see history."_s, card);
    this->labelRecentEmpty->setWordWrap(true);
    this->labelRecentEmpty->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; font-style: italic; "
                            "background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    vbox->addWidget(this->labelRecentEmpty);

    vbox->addStretch(1);

    // Capacity footer (workers + sessions) stays — it's the only static info
    // worth keeping per design feedback.
    auto *sep = new QFrame(card);
    sep->setFixedHeight(1);
    sep->setStyleSheet(
            QStringLiteral("background-color: rgba(4, 138, 191, 0.18); border: none;"));
    vbox->addWidget(sep);

    this->labelCapacity = new QLabel(u"Backend capacity: -"_s, card);
    this->labelCapacity->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    vbox->addWidget(this->labelCapacity);

    return card;
}

QFrame *DataHubWidget::buildStatusBar()
{
    // Bottom status strip — info COMPLEMENTARY to the backend toolbar at the
    // top (which already shows URL + Connected pill + workers/sessions text).
    // We avoid duplicating those readouts and instead surface counts that
    // aren't visible elsewhere on the hub:
    //   • Recent datasets count (with pinned subset if any)
    //   • Currently-running tasks
    //   • Sanity badge
    auto *bar = new QFrame(this);
    bar->setObjectName(u"statusBar"_s);
    bar->setStyleSheet(
            QStringLiteral(
                    "QFrame#statusBar {"
                    "  background-color: %1;"
                    "  border-top: 1px solid %2;"
                    "  border-radius: 0;"
                    "}"
                    "QFrame#statusBar QLabel { background: transparent; }")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                         QLatin1StringView(VisivoTheme::kOutline())));

    auto *hbox = new QHBoxLayout(bar);
    hbox->setContentsMargins(14, 6, 14, 6);
    hbox->setSpacing(14);

    const QString mutedStyle =
            QStringLiteral("color: %1; font-size: 11px;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));

    // Datasets readout: "14 datasets · 2 pinned" — populated in refreshStatus.
    // Repurposes the existing statusBarHealth pointer slot to keep the diff
    // small; the label is no longer about health state, it's about content.
    this->statusBarHealth = new QLabel(u"- datasets"_s, bar);
    this->statusBarHealth->setStyleSheet(mutedStyle);
    hbox->addWidget(this->statusBarHealth);

    auto pipe = [&]() {
        auto *p = new QLabel(u"·"_s, bar);
        p->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12px;")
                        .arg(QLatin1StringView(VisivoTheme::kSecondary())));
        hbox->addWidget(p);
    };

    pipe();
    // Running-tasks readout: "1 job running" / "no jobs" — keeps the user
    // aware of activity without scrolling to "Last 5 jobs".
    this->statusBarWorkers = new QLabel(u"no jobs"_s, bar);
    this->statusBarWorkers->setStyleSheet(mutedStyle);
    hbox->addWidget(this->statusBarWorkers);

    hbox->addStretch(1);

    // Sanity badge stays at the right edge; it's the one signal we DO want
    // duplicated because it's the global "is the backend behaving" indicator.
    this->statusBarSanity = VisivoTheme::makeBadge(
            u"sanity ok"_s, VisivoTheme::BadgeKind::Success, bar);
    this->statusBarSanity->setVisible(false);
    hbox->addWidget(this->statusBarSanity);

    return bar;
}

QFrame *DataHubWidget::buildActionGrid()
{
    auto *container = new QFrame(this);
    container->setStyleSheet(u"QFrame { background: transparent; border: none; }"_s);
    auto *grid = new QGridLayout(container);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(12);

    struct CardInfo {
        QString iconKind;
        QString title;
        QString desc;
    };

    const CardInfo cards[] = {
        { u"dataset"_s,   u"Open Remote Dataset"_s, u"Browse and open FITS images or spectral cubes via the backend."_s },
        { u"catalogue"_s, u"Open 3D Catalogue"_s,   u"Visualise astronomical catalogues (CSV / VOTable / FITS / speck / IPAC) in the 3D source viewer."_s },
        { u"vbt"_s,       u"Open VBT"_s,            u"Load a VisIVO Binary Table for point-cloud or volume rendering."_s },
        { u"hips"_s,      u"HiPS Viewer"_s,         u"Explore hierarchical progressive sky surveys."_s },
    };

    // Connect each card to the matching signal.
    void (DataHubWidget::*actions[])() = {
        &DataHubWidget::openDataRequested,
        &DataHubWidget::openCatalogueRequested,
        &DataHubWidget::openVbtRequested,
        &DataHubWidget::openHiPSRequested,
    };

    for (int i = 0; i < 4; ++i) {
        auto *card = makeActionCard(cards[i].iconKind, cards[i].title, cards[i].desc, container);
        auto action = actions[i];
        auto *filter = new datahub::CardClickFilter([this, action]() { emit (this->*action)(); }, card);
        card->installEventFilter(filter);
        grid->addWidget(card, 0, i);
        grid->setColumnStretch(i, 1);
    }

    return container;
}

QFrame *DataHubWidget::makeActionCard(const QString &iconKind, const QString &title,
                                      const QString &desc, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setMinimumHeight(112);
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
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHigh()),
                         QLatin1StringView(VisivoTheme::kSurfaceBright())));

    auto *hbox = new QHBoxLayout(card);
    hbox->setContentsMargins(20, 20, 20, 20);
    hbox->setSpacing(12);

    // Larger SVG icon on the left: primary visual anchor for the card.
    auto *iconLbl = new QLabel(card);
    iconLbl->setPixmap(actionCardIconPixmap(iconKind, QString::fromLatin1(VisivoTheme::kPrimary()), 48));
    iconLbl->setFixedSize(56, 56);
    iconLbl->setAlignment(Qt::AlignCenter);
    iconLbl->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    iconLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
    hbox->addWidget(iconLbl, 0, Qt::AlignTop);

    auto *textCol = new QVBoxLayout;
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(6);

    // Title — Lato Bold (brand heading weight for action card titles)
    auto *titleLbl = new QLabel(title, card);
    {
        QFont f(QStringLiteral("Lato"));
        f.setPixelSize(17);
        f.setWeight(QFont::Bold);
        titleLbl->setFont(f);
        titleLbl->setStyleSheet(
                QStringLiteral("color: %1; background: transparent; border: none;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        titleLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    textCol->addWidget(titleLbl);

    // Description — small, muted
    auto *descLbl = new QLabel(desc, card);
    descLbl->setWordWrap(true);
    descLbl->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; background: transparent; border: none;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    descLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
    descLbl->setMaximumHeight(36);
    descLbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    textCol->addWidget(descLbl);
    textCol->addStretch(1);

    hbox->addLayout(textCol, 1);
    return card;
}

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
                        .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHighest()),
                             QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    }
    if (this->labelStatusDetail) {
        this->labelStatusDetail->setText(u"Click \"Check Connection\" to verify backend reachability."_s);
    }
}

bool DataHubWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        // Search bar → command palette.
        if (auto *w = qobject_cast<QFrame *>(watched);
            w && w->objectName() == u"hubSearchBar"_s) {
            emit commandPaletteRequested();
            return true;
        }
        // Quick-action rows store their target signal in the "qaSignal" property
        // so we don't have to multiplex via subclassing.
        if (auto *w = qobject_cast<QFrame *>(watched);
            w && w->objectName() == u"qaRow"_s) {
            const QString sig = w->property("qaSignal").toString();
            if      (sig == u"openAnythingRequested"_s)  emit openAnythingRequested();
            else if (sig == u"openDataRequested"_s)      emit openDataRequested();
            else if (sig == u"openCatalogueRequested"_s) emit openCatalogueRequested();
            else if (sig == u"openVbtRequested"_s)       emit openVbtRequested();
            else if (sig == u"openHiPSRequested"_s)      emit openHiPSRequested();
            else if (sig == u"openVLKBRequested"_s)      emit openVLKBRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
