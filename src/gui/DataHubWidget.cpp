#include "DataHubWidget.h"
#include "app/BackendClient.h"
#include "app/RecentDatasetsManager.h"
#include "theme/VisivoTheme.h"

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
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QTimer>

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
                u"VisIVO Visual Analytics, crafted with ❤️ by VisIVOLab team"_s,
                content);
        footer->setWordWrap(true);
        footer->setStyleSheet(
                QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
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
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()),
                         QLatin1StringView(VisivoTheme::kSurfaceContainerHigh())));
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
                        .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        this->labelBackendUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        this->labelBackendUrl->setCursor(Qt::IBeamCursor);
        this->labelBackendUrl->setWordWrap(true);
        infoCol->addWidget(this->labelBackendUrl);
    }

    // Detail / error text
    this->labelStatusDetail = new QLabel(u"Set the backend URL in Edit → Settings."_s, card);
    this->labelStatusDetail->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
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
                "Open a dataset using the actions above."_s,
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

        // Type icon (24×24 — smaller than action cards).
        const auto ti = typeInfo(ds.type);
        auto *iconLbl = new QLabel(row);
        iconLbl->setPixmap(
                actionCardIconPixmap(ti.iconKind,
                                     QString::fromLatin1(VisivoTheme::kPrimaryDim()), 28));
        iconLbl->setFixedSize(32, 32);
        iconLbl->setAlignment(Qt::AlignCenter);
        iconLbl->setStyleSheet(u"background: transparent; border: none;"_s);
        iconLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        hbox->addWidget(iconLbl, 0, Qt::AlignVCenter);

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
                        .arg(QLatin1StringView(VisivoTheme::kPrimaryDim())));
        metricBox->addWidget(lbl);
        this->labelWorkers = new QLabel(u"—"_s, card);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(32);
            f.setWeight(QFont::Black);
            this->labelWorkers->setFont(f);
            this->labelWorkers->setStyleSheet(
                    QStringLiteral("color: %1; background: transparent;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
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
                        .arg(QLatin1StringView(VisivoTheme::kPrimaryDim())));
        metricBox->addWidget(lbl);
        this->labelSessions = new QLabel(u"—"_s, card);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(32);
            f.setWeight(QFont::Black);
            this->labelSessions->setFont(f);
            this->labelSessions->setStyleSheet(
                    QStringLiteral("color: %1; background: transparent;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
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
                        .arg(QLatin1StringView(VisivoTheme::kPrimaryDim())));
        metricBox->addWidget(lbl);
        this->labelTaskRegistry = new QLabel(u"—"_s, card);
        {
            QFont f(QStringLiteral("Lato"));
            f.setPixelSize(32);
            f.setWeight(QFont::Black);
            this->labelTaskRegistry->setFont(f);
            this->labelTaskRegistry->setStyleSheet(
                    QStringLiteral("color: %1; background: transparent;")
                            .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
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
                QStringLiteral("color: %1; font-size: 11px; "
                                "font-style: italic; background: transparent;")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
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
                        .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHighest()),
                             QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
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
                            .arg(QLatin1StringView(VisivoTheme::kErrorContainer()),
                                 QLatin1StringView(VisivoTheme::kOnErrorContainer())));
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
