#include "DataHubWidget.h"
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
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTimer>
#include <QtConcurrentRun>

#include <cmath>

using namespace Qt::StringLiterals;

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

// Returns a human-readable relative time string for a past QDateTime.
QString timeAgo(const QDateTime &dt)
{
    const qint64 secs = dt.secsTo(QDateTime::currentDateTime());
    if (secs < 5)   return QStringLiteral("just now");
    if (secs < 60)  return QStringLiteral("%1 sec ago").arg(secs);
    if (secs < 3600) return QStringLiteral("%1 min ago").arg(secs / 60);
    if (secs < 86400) return QStringLiteral("%1h ago").arg(secs / 3600);
    if (secs < 604800) return QStringLiteral("%1d ago").arg(secs / 86400);
    if (secs < 2592000) return QStringLiteral("%1w ago").arg(secs / 604800);
    return dt.toString(QStringLiteral("MMM d, yyyy"));
}

// Returns a short uppercase label for a dataset type, plus an icon kind for rendering.
struct TypeInfo { QString label; QString iconKind; };
TypeInfo typeInfo(const QString &type)
{
    if (type == QLatin1StringView("cube"))       return { QStringLiteral("CUBE"),    QStringLiteral("dataset")   };
    if (type == QLatin1StringView("image"))      return { QStringLiteral("IMAGE"),   QStringLiteral("dataset")   };
    if (type == QLatin1StringView("catalogue"))  return { QStringLiteral("CAT"),     QStringLiteral("catalogue") };
    if (type == QLatin1StringView("vbt"))        return { QStringLiteral("VBT"),     QStringLiteral("vbt")       };
    if (type == QLatin1StringView("vbt-volume")) return { QStringLiteral("VBT-VOL"), QStringLiteral("vbt")       };
    if (type == QLatin1StringView("hips"))       return { QStringLiteral("HiPS"),    QStringLiteral("hips")      };
    return { type.toUpper(), QStringLiteral("dataset") };
}

} // namespace

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

// ── rebuildOnThemeChange ─────────────────────────────────────────────────────

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
                u"Backend-authoritative workflow — scientific operations run on remote nodes."_s,
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

// ── buildBackendCard (Layout B: thin toolbar with path-pill) ─────────────────

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

// ── buildSearchBar (Mission Control: global ⌘K search at top) ────────────────

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

// ── buildQuickActionsList (Mission Control: compact vertical list) ───────────

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
    };
    const std::array<Spec, 5> specs{
            Spec{u"dataset"_s,   u"Open Remote Dataset…"_s,
                 u"Browse FITS on the backend filesystem"_s, u"⌘O"_s, "openDataRequested"},
            Spec{u"catalogue"_s, u"Open 3D Catalogue…"_s,
                 u"CSV / VOTable in the source viewer"_s, QString(),  "openCatalogueRequested"},
            Spec{u"vbt"_s,       u"Open VBT…"_s,
                 u"VisIVO Binary Table — points / volumes"_s, u"⇧⌘B"_s, "openVbtRequested"},
            // Sky-map engines: both open as their own viewer tabs.
            Spec{u"hips"_s,      u"HiPS Viewer"_s,
                 u"Hierarchical sky surveys (DSS, 2MASS, …)"_s, QString(),
                 "openHiPSRequested"},
            Spec{u"hips"_s,      u"VLKB Inventory"_s,
                 u"Browse VLKB sources & add as image layers"_s, QString(),
                 "openVLKBRequested"},
    };

    for (const auto &s : specs) {
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

// ── buildLast5JobsPanel (extracted from old System Insights) ─────────────────

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

    this->labelCapacity = new QLabel(u"Backend capacity: —"_s, card);
    this->labelCapacity->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    vbox->addWidget(this->labelCapacity);

    return card;
}

// ── buildStatusBar (Layout B: thin bottom strip with health/session/sanity) ──

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
    this->statusBarHealth = new QLabel(u"— datasets"_s, bar);
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

// ── buildActionGrid ───────────────────────────────────────────────────────────

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
        { u"catalogue"_s, u"Open 3D Catalogue"_s,   u"Visualise astronomical CSV catalogues in the 3D source viewer."_s },
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
        auto *filter = new CardClickFilter([this, action]() { emit (this->*action)(); }, card);
        card->installEventFilter(filter);
        grid->addWidget(card, 0, i);
        grid->setColumnStretch(i, 1);
    }

    return container;
}

// ── makeActionCard ────────────────────────────────────────────────────────────

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

// ── buildRecentDatasetsPanel ──────────────────────────────────────────────────

QFrame *DataHubWidget::buildRecentDatasetsPanel()
{
    auto *card = VisivoTheme::makeCard(this);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(10);

    // Header row: section label (left) — future "Clear all" button goes here (right).
    auto *header = new QHBoxLayout;
    header->addWidget(VisivoTheme::makeSectionLabel(u"Recent Datasets"_s, card));
    header->addStretch(1);
    vbox->addLayout(header);

    // Scrollable item list.  The actual items are injected by refreshRecentDatasets().
    auto *scrollArea = new QScrollArea(card);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setStyleSheet(u"QScrollArea { background: transparent; border: none; }"_s);

    auto *itemContainer = new QWidget(scrollArea);
    itemContainer->setStyleSheet(u"background: transparent;"_s);
    this->recentItemsLayout = new QVBoxLayout(itemContainer);
    this->recentItemsLayout->setContentsMargins(0, 0, 0, 0);
    this->recentItemsLayout->setSpacing(6);
    this->recentItemsLayout->addStretch(1); // keeps items pushed to the top

    scrollArea->setWidget(itemContainer);
    scrollArea->setMinimumHeight(140);
    vbox->addWidget(scrollArea, 1);

    // Populate once from the current manager state (may be empty at startup
    // if manager is not yet set; a proper refresh happens in setRecentDatasetsManager).
    refreshRecentDatasets();

    return card;
}

// ── setRecentDatasetsManager ──────────────────────────────────────────────────

void DataHubWidget::setRecentDatasetsManager(RecentDatasetsManager *manager)
{
    this->recentMgr = manager;
    if (manager) {
        QObject::connect(manager, &RecentDatasetsManager::changed,
                         this, &DataHubWidget::refreshRecentDatasets);
    }
    refreshRecentDatasets();
}

// ── refreshRecentDatasets ─────────────────────────────────────────────────────

void DataHubWidget::refreshRecentDatasets()
{
    if (!this->recentItemsLayout) {
        return;
    }

    // Remove all items except the trailing stretch (always the last item).
    while (this->recentItemsLayout->count() > 1) {
        auto *item = this->recentItemsLayout->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const QList<RecentDataset> datasets =
            this->recentMgr ? this->recentMgr->datasets() : QList<RecentDataset>{};

    if (datasets.isEmpty()) {
        // Empty-state label.
        auto *empty = new QLabel(
                u"No recently opened datasets.\n"
                u"Open a dataset using the actions above."_s,
                nullptr);
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        empty->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12px; background: transparent; "
                                "padding: 16px 8px;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        this->recentItemsLayout->insertWidget(0, empty);
        return;
    }

    // Build one item frame per recent dataset.
    for (const auto &ds : datasets) {
        auto *row = new QFrame(nullptr);
        row->setAttribute(Qt::WA_Hover);
        row->setCursor(Qt::PointingHandCursor);
        // Idle row tint flips between a faint white-on-dark overlay and a
        // faint black-on-light overlay so the row separation is visible in
        // either theme without dominating the brand blue accent.
        const QLatin1StringView idleTint = VisivoTheme::isDark()
                ? "rgba(255,255,255,0.02)"_L1
                : "rgba(0,0,0,0.03)"_L1;
        row->setStyleSheet(
                QStringLiteral(
                        "QFrame {"
                        "  background-color: %1;"
                        "  border: 1px solid rgba(4,138,191,0.10);"
                        "  border-radius: 8px;"
                        "}"
                        "QFrame:hover {"
                        "  background-color: rgba(4,138,191,0.09);"
                        "  border-color: rgba(4,138,191,0.28);"
                        "}")
                        .arg(idleTint));

        auto *hbox = new QHBoxLayout(row);
        hbox->setContentsMargins(12, 10, 12, 10);
        hbox->setSpacing(10);

        // Mission Control thumbnail (96×64). Falls back to a typed gradient
        // placeholder if no real preview has been captured yet — the
        // ThumbnailCache populates the on-disk PNG when the user closes the
        // matching viewer, so the panel fills in over normal use.
        const auto ti = typeInfo(ds.type);
        auto *thumb = new QLabel(row);
        thumb->setFixedSize(ThumbnailCache::kDefaultWidth,
                            ThumbnailCache::kDefaultHeight);
        thumb->setAlignment(Qt::AlignCenter);
        QPixmap thumbPix = ThumbnailCache::load(ds.id);
        if (thumbPix.isNull()) {
            thumbPix = ThumbnailCache::placeholder(ds.type);
        }
        thumb->setPixmap(thumbPix);
        thumb->setStyleSheet(
                QStringLiteral(
                        "background: #04060a;"
                        "border: 1px solid %1;"
                        "border-radius: 4px;")
                        .arg(QLatin1StringView(VisivoTheme::kOutlineVariant())));
        thumb->setAttribute(Qt::WA_TransparentForMouseEvents);
        hbox->addWidget(thumb, 0, Qt::AlignVCenter);

        // Text column: display name (bold) + type/source (small).
        auto *textCol = new QVBoxLayout;
        textCol->setContentsMargins(0, 0, 0, 0);
        textCol->setSpacing(2);

        auto *nameLbl = new QLabel(ds.displayName, row);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(13);
            f.setWeight(QFont::DemiBold);
            nameLbl->setFont(f);
        }
        nameLbl->setStyleSheet(
                QStringLiteral("color: %1; background: transparent; border: none;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        nameLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameLbl->setToolTip(ds.id); // full path on hover
        textCol->addWidget(nameLbl);

        auto *metaLbl = new QLabel(
                QStringLiteral("%1 · %2").arg(ti.label, ds.source), row);
        metaLbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; background: transparent; border: none;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        metaLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        textCol->addWidget(metaLbl);

        hbox->addLayout(textCol, 1);

        // Relative timestamp (right-aligned, muted).
        auto *timeLbl = new QLabel(timeAgo(ds.lastOpened), row);
        timeLbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; background: transparent; border: none;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        timeLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        timeLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        hbox->addWidget(timeLbl, 0, Qt::AlignVCenter);

        // Click → emit reopenDatasetRequested.
        RecentDataset dsCopy = ds; // capture by value
        auto *filter = new CardClickFilter(
                [this, dsCopy]() { emit reopenDatasetRequested(dsCopy); }, row);
        row->installEventFilter(filter);

        // Insert before the trailing stretch.
        const int insertPos = this->recentItemsLayout->count() - 1;
        this->recentItemsLayout->insertWidget(insertPos, row);
    }
}

// ── Workspace Exports panel ─────────────────────────────────────────────────
//
// Lifecycle:
//   * buildWorkspaceExportsPanel() — built once in buildUi(), creates the
//     card + a refresh button + an empty item list. Schedules a first
//     refresh on the next event-loop tick so the panel is populated by
//     the time the user looks at it.
//   * refreshWorkspaceExports() — async list() call to the backend; on
//     completion, clears and rebuilds the item rows. Re-called from
//     refreshStatus() so the panel stays current as cube viewers push
//     new artefacts in.
//
// Per-row buttons:
//   * "Open in VisIVO" — emits openWorkspaceExportRequested(absolutePath)
//   * "Download…"      — native Save As + downloadExport() in a worker
//   * "Delete"         — confirm + deleteExport() + refresh

namespace {
QString formatExportSize(qint64 bytes)
{
    if (bytes < 1024)         return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)  return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < qint64(1024) * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    return QStringLiteral("%1 GB").arg(bytes / (qint64(1024) * 1024 * 1024));
}

QString formatExportTime(const QString &iso8601)
{
    const QDateTime t = QDateTime::fromString(iso8601, Qt::ISODate);
    if (!t.isValid()) return iso8601;
    return t.toLocalTime().toString(u"yyyy-MM-dd hh:mm"_s);
}
} // namespace

QFrame *DataHubWidget::buildWorkspaceExportsPanel()
{
    auto *card = VisivoTheme::makeCard(this);
    m_exportsCard = card;

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->addWidget(VisivoTheme::makeSectionLabel(
            u"Workspace Exports"_s, card));
    m_exportsCountLabel = new QLabel(QString(), card);
    m_exportsCountLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; "
                           "background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    header->addWidget(m_exportsCountLabel);
    header->addStretch(1);

    auto *refreshBtn = new QPushButton(u"Refresh"_s, card);
    refreshBtn->setCursor(Qt::PointingHandCursor);
    QObject::connect(refreshBtn, &QPushButton::clicked, this,
                     &DataHubWidget::refreshWorkspaceExports);
    header->addWidget(refreshBtn);
    vbox->addLayout(header);

    auto *scroll = new QScrollArea(card);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(u"QScrollArea { background: transparent; border: none; }"_s);

    auto *itemContainer = new QWidget(scroll);
    itemContainer->setStyleSheet(u"background: transparent;"_s);
    m_exportsItemsLayout = new QVBoxLayout(itemContainer);
    m_exportsItemsLayout->setContentsMargins(0, 0, 0, 0);
    m_exportsItemsLayout->setSpacing(6);

    m_exportsEmptyLabel = new QLabel(
            u"No exports yet. Use *Tools → Export Sub-Cube / Moment as FITS…* "
            u"from a cube viewer to populate this list."_s,
            itemContainer);
    m_exportsEmptyLabel->setWordWrap(true);
    m_exportsEmptyLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px; "
                           "background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    m_exportsItemsLayout->addWidget(m_exportsEmptyLabel);
    m_exportsItemsLayout->addStretch(1);

    scroll->setWidget(itemContainer);
    scroll->setMinimumHeight(140);
    vbox->addWidget(scroll, 1);

    // First refresh deferred to next tick so the backend URL is already set
    // by MainWindow's wiring.
    QTimer::singleShot(0, this, &DataHubWidget::refreshWorkspaceExports);

    return card;
}

void DataHubWidget::refreshWorkspaceExports()
{
    if (!m_exportsItemsLayout) return;
    if (this->backendUrl.isEmpty()) return;   // nothing to call yet

    const QString url = this->backendUrl;
    const QString tok = this->backendToken;

    auto *watcher = new QFutureWatcher<BackendWorkspaceExportsList>(this);
    QPointer<DataHubWidget> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendWorkspaceExportsList>::finished, this,
                     [self, watcher]() {
                         const auto listing = watcher->result();
                         watcher->deleteLater();
                         if (!self) return;

                         // Wipe every old row (keep stretch + empty label).
                         QVBoxLayout *lay = self->m_exportsItemsLayout;
                         if (!lay) return;
                         while (lay->count() > 0) {
                             QLayoutItem *li = lay->takeAt(0);
                             if (auto *w = li->widget()) {
                                 if (w == self->m_exportsEmptyLabel) {
                                     w->hide();
                                     // Re-attach later so we can show it
                                     // when the list comes back empty.
                                 } else {
                                     w->deleteLater();
                                 }
                             }
                             delete li;
                         }

                         if (!listing.valid) {
                             self->m_exportsCountLabel->setText(
                                     u"(failed: %1)"_s.arg(listing.error));
                             if (self->m_exportsEmptyLabel) {
                                 self->m_exportsEmptyLabel->show();
                                 lay->addWidget(self->m_exportsEmptyLabel);
                             }
                             lay->addStretch(1);
                             return;
                         }

                         self->m_exportsCountLabel->setText(
                                 listing.entries.isEmpty()
                                 ? QString()
                                 : u"(%1)"_s.arg(listing.entries.size()));

                         if (listing.entries.isEmpty()) {
                             if (self->m_exportsEmptyLabel) {
                                 self->m_exportsEmptyLabel->show();
                                 lay->addWidget(self->m_exportsEmptyLabel);
                             }
                             lay->addStretch(1);
                             return;
                         }

                         for (const auto &entry : listing.entries) {
                             auto *row = new QFrame();
                             row->setStyleSheet(QStringLiteral(
                                     "QFrame {"
                                     "  background: %1;"
                                     "  border: 1px solid %2;"
                                     "  border-radius: 6px;"
                                     "}")
                                     .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                                          QLatin1StringView(VisivoTheme::kOutline())));
                             auto *h = new QHBoxLayout(row);
                             h->setContentsMargins(10, 6, 10, 6);
                             h->setSpacing(10);

                             auto *name = new QLabel(entry.filename, row);
                             name->setStyleSheet(
                                     QStringLiteral("color: %1; font-weight: 600; "
                                                    "background: transparent; border: none;")
                                             .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
                             name->setTextInteractionFlags(Qt::TextSelectableByMouse);
                             h->addWidget(name, 1);

                             auto *meta = new QLabel(
                                     u"%1 · %2"_s
                                     .arg(formatExportSize(entry.size),
                                          formatExportTime(entry.modifiedTime)),
                                     row);
                             meta->setStyleSheet(
                                     QStringLiteral("color: %1; font-size: 10px; "
                                                    "background: transparent; border: none;")
                                             .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
                             h->addWidget(meta);

                             auto *openBtn     = new QPushButton(u"Open"_s, row);
                             auto *downloadBtn = new QPushButton(u"Download…"_s, row);
                             auto *deleteBtn   = new QPushButton(u"Delete"_s, row);
                             for (auto *b : { openBtn, downloadBtn, deleteBtn }) {
                                 b->setCursor(Qt::PointingHandCursor);
                                 b->setStyleSheet(QStringLiteral(
                                         "QPushButton {"
                                         " padding: 4px 10px; font-size: 11px;"
                                         " border: 1px solid %1; border-radius: 4px;"
                                         " background: transparent; color: %2;"
                                         "}"
                                         "QPushButton:hover { background: %3; }")
                                         .arg(QLatin1StringView(VisivoTheme::kOutline()),
                                              QLatin1StringView(VisivoTheme::kOnSurface()),
                                              QLatin1StringView(VisivoTheme::kSurfaceContainerHigh())));
                                 h->addWidget(b);
                             }

                             const QString abs = entry.absolutePath;
                             const QString fn  = entry.filename;
                             QObject::connect(openBtn, &QPushButton::clicked, self.data(),
                                              [self, abs]() {
                                                  if (!self) return;
                                                  emit self->openWorkspaceExportRequested(abs);
                                              });
                             QObject::connect(downloadBtn, &QPushButton::clicked, self.data(),
                                              [self, fn]() {
                                                  if (!self) return;
                                                  const QString defaultDir =
                                                          QStandardPaths::writableLocation(
                                                                  QStandardPaths::DocumentsLocation);
                                                  const QString suggested =
                                                          QDir(defaultDir.isEmpty()
                                                               ? QDir::homePath() : defaultDir)
                                                                  .filePath(fn);
                                                  const QString localPath = QFileDialog::getSaveFileName(
                                                          self, u"Download Workspace Export"_s,
                                                          suggested,
                                                          u"FITS files (*.fits *.fit);;All files (*)"_s);
                                                  if (localPath.isEmpty()) return;
                                                  const QString url2 = self->backendUrl;
                                                  const QString tok2 = self->backendToken;
                                                  QPointer<DataHubWidget> guard(self);
                                                  auto *dlWatcher = new QFutureWatcher<QString>(self);
                                                  QObject::connect(
                                                          dlWatcher, &QFutureWatcher<QString>::finished, self.data(),
                                                          [guard, dlWatcher, localPath]() {
                                                              const QString err = dlWatcher->result();
                                                              dlWatcher->deleteLater();
                                                              if (!guard) return;
                                                              if (err.isEmpty()) {
                                                                  QMessageBox::information(
                                                                          guard, u"Download"_s,
                                                                          u"Saved to:\n%1"_s.arg(localPath));
                                                              } else {
                                                                  QMessageBox::warning(
                                                                          guard, u"Download"_s,
                                                                          u"Download failed:\n%1"_s.arg(err));
                                                              }
                                                          });
                                                  dlWatcher->setFuture(QtConcurrent::run(
                                                          [url2, tok2, fn, localPath]() {
                                                              BackendClient c(url2, tok2);
                                                              QString err;
                                                              c.downloadExport(fn, localPath, err);
                                                              return err;
                                                          }));
                                              });
                             QObject::connect(deleteBtn, &QPushButton::clicked, self.data(),
                                              [self, fn]() {
                                                  if (!self) return;
                                                  const auto reply = QMessageBox::question(
                                                          self, u"Delete export"_s,
                                                          u"Remove '%1' from the Workspace Exports?\n"
                                                          u"This cannot be undone."_s.arg(fn));
                                                  if (reply != QMessageBox::Yes) return;
                                                  const QString url2 = self->backendUrl;
                                                  const QString tok2 = self->backendToken;
                                                  QPointer<DataHubWidget> guard(self);
                                                  auto *delWatcher = new QFutureWatcher<BackendWorkspaceDeleteResult>(self);
                                                  QObject::connect(
                                                          delWatcher, &QFutureWatcher<BackendWorkspaceDeleteResult>::finished,
                                                          self.data(), [guard, delWatcher]() {
                                                              const auto r = delWatcher->result();
                                                              delWatcher->deleteLater();
                                                              if (!guard) return;
                                                              if (!r.valid) {
                                                                  QMessageBox::warning(
                                                                          guard, u"Delete export"_s,
                                                                          u"Could not delete:\n%1"_s.arg(r.error));
                                                              }
                                                              guard->refreshWorkspaceExports();
                                                          });
                                                  delWatcher->setFuture(QtConcurrent::run(
                                                          [url2, tok2, fn]() {
                                                              BackendClient c(url2, tok2);
                                                              return c.deleteExport(fn);
                                                          }));
                                              });

                             lay->addWidget(row);
                         }
                         lay->addStretch(1);
                     });
    watcher->setFuture(QtConcurrent::run([url, tok]() {
        BackendClient c(url, tok);
        return c.listExports();
    }));
}

// ── buildSystemInsightsPanel ──────────────────────────────────────────────────

QFrame *DataHubWidget::buildSystemInsightsPanel()
{
    auto *card = VisivoTheme::makeCard(this, true /* elevated */);
    card->setMinimumWidth(220);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(14);

    vbox->addWidget(VisivoTheme::makeSectionLabel(u"System Insights"_s, card));

    // ── Live activity: running tasks count + small caption ───────────────────
    // Replaces the old static "Workers/Sessions/Task Registry" trio that didn't
    // change during a session. Now shows what the backend is *currently doing*.
    {
        auto *metricBox = new QVBoxLayout;
        metricBox->setSpacing(2);

        auto *lbl = new QLabel(u"Active operations"_s, card);
        lbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; letter-spacing: 0.1em; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        metricBox->addWidget(lbl);

        this->labelRunningTasks = new QLabel(u"—"_s, card);
        QFont f(QStringLiteral("Lato"));
        f.setPixelSize(32);
        f.setWeight(QFont::Black);
        this->labelRunningTasks->setFont(f);
        this->labelRunningTasks->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        metricBox->addWidget(this->labelRunningTasks);

        // Sub-caption that switches between "idle" and "in flight…" depending
        // on running count. Stays a fixed-height label so the panel doesn't
        // jump as state changes.
        this->labelRunningCaption = new QLabel(u"Idle"_s, card);
        this->labelRunningCaption->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        metricBox->addWidget(this->labelRunningCaption);
        vbox->addLayout(metricBox);
    }

    // ── Recent operations list ───────────────────────────────────────────────
    // Up to N most recently finished tasks, each rendered as
    // "<icon> <operation>   <duration> · <age>". Populated in refreshStatus
    // from health.recentTasks. Hidden entirely when the list is empty.
    {
        auto *recentHdr = new QLabel(u"RECENT"_s, card);
        recentHdr->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; font-weight: 700; "
                                "letter-spacing: 0.18em; "
                                "background: transparent; margin-top: 4px;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        recentHdr->setObjectName(u"recentTasksHeader"_s);
        vbox->addWidget(recentHdr);
        this->labelRecentHeader = recentHdr;

        this->recentTasksContainer = new QWidget(card);
        this->recentTasksContainer->setStyleSheet(u"background: transparent;"_s);
        auto *recentLayout = new QVBoxLayout(this->recentTasksContainer);
        recentLayout->setContentsMargins(0, 0, 0, 0);
        recentLayout->setSpacing(4);
        vbox->addWidget(this->recentTasksContainer);

        // Empty-state placeholder shown when no recent tasks are reported.
        this->labelRecentEmpty = new QLabel(
                u"No operations yet. Open a dataset to start."_s, card);
        this->labelRecentEmpty->setWordWrap(true);
        this->labelRecentEmpty->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; font-style: italic; "
                                "background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        vbox->addWidget(this->labelRecentEmpty);
    }

    // ── Separator + Backend capacity footer ──────────────────────────────────
    {
        auto *sep = new QFrame(card);
        sep->setFixedHeight(1);
        sep->setStyleSheet(
                QStringLiteral("background-color: rgba(4, 138, 191, 0.18); border: none;"));
        vbox->addWidget(sep);

        this->labelCapacity = new QLabel(u"Backend capacity: —"_s, card);
        this->labelCapacity->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        vbox->addWidget(this->labelCapacity);
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
                        .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHighest()),
                             QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    }
    if (this->labelStatusDetail) {
        this->labelStatusDetail->setText(u"Click \"Check Connection\" to verify backend reachability."_s);
    }
}

// ── eventFilter (Mission Control click dispatch) ─────────────────────────────

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
            if      (sig == u"openDataRequested"_s)      emit openDataRequested();
            else if (sig == u"openCatalogueRequested"_s) emit openCatalogueRequested();
            else if (sig == u"openVbtRequested"_s)       emit openVbtRequested();
            else if (sig == u"openHiPSRequested"_s)      emit openHiPSRequested();
            else if (sig == u"openVLKBRequested"_s)      emit openVLKBRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ── refreshStatus ─────────────────────────────────────────────────────────────

void DataHubWidget::refreshStatus()
{
    // Don't refresh status while a modal dialog is up — see the same guard
    // in MainWindow::pollBackendDiagnosticsOnce(). Updating the badge's
    // stylesheet here causes a parent-window event burst on macOS that
    // dismisses native save dialogs spawned by secondary windows.
    if (QApplication::activeModalWidget() != nullptr) return;
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
                            .arg(QLatin1StringView(VisivoTheme::kWarningContainer()),
                                 QLatin1StringView(VisivoTheme::kOnWarningContainer())));
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
                            .arg(QLatin1StringView(VisivoTheme::kTertiaryContainer()),
                                 QLatin1StringView(VisivoTheme::kOnTertiaryFixed())));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    QStringLiteral("Backend reachable — workers: %1, sessions: %2")
                            .arg(health.workers)
                            .arg(health.activeSessions));
        }
        // We're in the `health.ok` branch — every value is authoritative.
        // Populate the live activity readouts from the new health snapshot.
        this->updateSystemInsights(health);

        // Same cadence keeps the Workspace Exports list in sync — new
        // artefacts pushed by cube viewers (Tools → Export …) appear
        // here without the user having to click Refresh.
        this->refreshWorkspaceExports();

        // Bottom status bar: counts that aren't already in the top toolbar
        // (dataset count + currently-running tasks + sanity).
        const int datasetCount = this->recentMgr ? this->recentMgr->datasets().size() : 0;
        if (this->statusBarHealth) {
            this->statusBarHealth->setText(
                    datasetCount == 1
                            ? QStringLiteral("1 dataset")
                            : QStringLiteral("%1 datasets").arg(datasetCount));
            this->statusBarHealth->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarWorkers) {
            const int n = health.runningTasks;
            this->statusBarWorkers->setText(
                    n == 0 ? u"no jobs running"_s
                           : (n == 1 ? u"1 job running"_s
                                     : QStringLiteral("%1 jobs running").arg(n)));
            this->statusBarWorkers->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(
                                    n > 0 ? VisivoTheme::kRunning()
                                          : VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarSanity) {
            this->statusBarSanity->setText(u"sanity ok"_s);
            this->statusBarSanity->setVisible(true);
        }

        qDebug().noquote()
                << QStringLiteral("[DataHub] backend reachable — workers=%1 running=%2 sessions=%3")
                           .arg(health.workers)
                           .arg(health.runningTasks)
                           .arg(health.activeSessions);
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
                            .arg(QLatin1StringView(VisivoTheme::kErrorContainer()),
                                 QLatin1StringView(VisivoTheme::kOnErrorContainer())));
        }
        if (this->labelStatusDetail) {
            this->labelStatusDetail->setText(
                    health.error.isEmpty()
                            ? u"No response from backend."_s
                            : health.error);
        }
        // Reset live activity readouts to "unknown" placeholder state.
        if (this->labelRunningTasks)   this->labelRunningTasks->setText(u"—"_s);
        if (this->labelRunningCaption) this->labelRunningCaption->setText(u"Backend unreachable"_s);
        if (this->labelCapacity)       this->labelCapacity->setText(u"Backend capacity: —"_s);
        this->clearRecentTasksUi();

        // Bottom status bar: dataset count is still meaningful even when the
        // backend is unreachable; the running-jobs cell becomes "—" and the
        // sanity badge hides.
        const int datasetCount = this->recentMgr ? this->recentMgr->datasets().size() : 0;
        if (this->statusBarHealth) {
            this->statusBarHealth->setText(
                    datasetCount == 1 ? QStringLiteral("1 dataset")
                                       : QStringLiteral("%1 datasets").arg(datasetCount));
            this->statusBarHealth->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarWorkers) {
            this->statusBarWorkers->setText(u"jobs —"_s);
            this->statusBarWorkers->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        }
        if (this->statusBarSanity) this->statusBarSanity->setVisible(false);
        qDebug().noquote()
                << QStringLiteral("[DataHub] backend unreachable — %1").arg(health.error);
    }

    if (this->btnCheckConnection) {
        this->btnCheckConnection->setEnabled(true);
        this->btnCheckConnection->setText(u"Check Connection"_s);
    }
}

// ── System Insights helpers ──────────────────────────────────────────────────

namespace {

// Compact "12s", "1m", "3m 24s" form for showing a duration in the recent
// list. Anything past an hour collapses to "Xh".
QString formatDurationShort(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return u"—"_s;
    if (seconds < 1.0)   return QStringLiteral("%1ms").arg(int(seconds * 1000.0));
    if (seconds < 60.0)  return QStringLiteral("%1s").arg(seconds, 0, 'f', seconds < 10.0 ? 1 : 0);
    if (seconds < 3600.0) {
        const int m = int(seconds / 60.0);
        const int s = int(seconds) % 60;
        return s == 0 ? QStringLiteral("%1m").arg(m)
                      : QStringLiteral("%1m %2s").arg(m).arg(s);
    }
    return QStringLiteral("%1h").arg(seconds / 3600.0, 0, 'f', 1);
}

// "just now", "12s ago", "3m ago", "2h ago" — for the "age" of a finished task.
QString formatAgoShort(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return u""_s;
    if (seconds < 5.0)    return u"just now"_s;
    if (seconds < 60.0)   return QStringLiteral("%1s ago").arg(int(seconds));
    if (seconds < 3600.0) return QStringLiteral("%1m ago").arg(int(seconds / 60.0));
    if (seconds < 86400.0) return QStringLiteral("%1h ago").arg(int(seconds / 3600.0));
    return QStringLiteral("%1d ago").arg(int(seconds / 86400.0));
}

// Single status pill: ✓ for success, ✗ for failure, ◌ for cancelled.
QString statusGlyph(const QString &status)
{
    if (status == u"failed"_s)    return QStringLiteral("✗");
    if (status == u"cancelled"_s) return QStringLiteral("◌");
    return QStringLiteral("✓");
}

QString statusColor(const QString &status)
{
    if (status == u"failed"_s)    return u"#EF5350"_s;
    if (status == u"cancelled"_s) return u"#FFA726"_s;
    return u"#4CAF50"_s; // done
}

} // namespace

void DataHubWidget::clearRecentTasksUi()
{
    if (!this->recentTasksContainer) return;
    auto *layout = this->recentTasksContainer->layout();
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (auto *w = item->widget()) w->deleteLater();
        delete item;
    }
    if (this->labelRecentEmpty)  this->labelRecentEmpty->setVisible(true);
    if (this->labelRecentHeader) this->labelRecentHeader->setVisible(false);
    this->recentTasksContainer->setVisible(false);
}

void DataHubWidget::updateSystemInsights(const BackendHealthResult &health)
{
    // Running counter + caption switching between "Idle" and "in flight…".
    if (this->labelRunningTasks) {
        this->labelRunningTasks->setText(QString::number(health.runningTasks));
    }
    if (this->labelRunningCaption) {
        this->labelRunningCaption->setText(
                health.runningTasks == 0
                        ? u"Idle — backend ready."_s
                        : QStringLiteral("%1 operation%2 in flight…")
                                  .arg(health.runningTasks)
                                  .arg(health.runningTasks == 1 ? u""_s : u"s"_s));
    }

    // Backend capacity footer (workers + active sessions when > 0).
    if (this->labelCapacity) {
        QString text = QStringLiteral("Backend capacity: %1 worker%2")
                               .arg(health.workers)
                               .arg(health.workers == 1 ? u""_s : u"s"_s);
        if (health.activeSessions > 0) {
            text += QStringLiteral(" · %1 session%2")
                            .arg(health.activeSessions)
                            .arg(health.activeSessions == 1 ? u""_s : u"s"_s);
        }
        this->labelCapacity->setText(text);
    }

    // Recent operations list — rebuild from scratch each refresh so order
    // and ages stay accurate. Backend caps the list at 5 entries.
    if (!this->recentTasksContainer) return;
    auto *layout = qobject_cast<QVBoxLayout *>(this->recentTasksContainer->layout());
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (auto *w = item->widget()) w->deleteLater();
        delete item;
    }

    const bool hasItems = !health.recentTasks.empty();
    if (this->labelRecentEmpty)  this->labelRecentEmpty->setVisible(!hasItems);
    if (this->labelRecentHeader) this->labelRecentHeader->setVisible(hasItems);
    this->recentTasksContainer->setVisible(hasItems);

    for (const auto &task : health.recentTasks) {
        auto *row = new QWidget(this->recentTasksContainer);
        row->setStyleSheet(u"background: transparent;"_s);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto *icon = new QLabel(statusGlyph(task.status), row);
        icon->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12px; font-weight: 700; "
                                "background: transparent;").arg(statusColor(task.status)));
        icon->setFixedWidth(14);
        rowLayout->addWidget(icon);

        QString opLabel = task.operation;
        if (task.cacheHit) {
            opLabel += QStringLiteral(" (cache)");
        }
        auto *op = new QLabel(opLabel, row);
        op->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        rowLayout->addWidget(op, 1);

        const QString meta = QStringLiteral("%1 · %2")
                                     .arg(formatDurationShort(task.durationSeconds),
                                          formatAgoShort(task.ageSeconds));
        auto *metaLbl = new QLabel(meta, row);
        metaLbl->setStyleSheet(
                QStringLiteral("color: %1; font-size: 10px; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        rowLayout->addWidget(metaLbl);

        layout->addWidget(row);
    }
}
