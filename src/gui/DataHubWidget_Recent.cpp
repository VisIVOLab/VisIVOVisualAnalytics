#include "DataHubWidget.h"
#include "CardClickFilter.h"
#include "ElideOnResize.h"
#include "app/BackendClient.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "app/ThumbnailCache.h"
#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFontMetrics>
#include <QObject>
#include <QPointer>
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

// ── DataHubWidget: Recent (extracted from DataHubWidget.cpp) ──

namespace {

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

struct TypeInfo { QString label; QString iconKind; };

TypeInfo typeInfo(const QString &type)
{
    if (type == QLatin1StringView("cube"))       return { QStringLiteral("CUBE"),    QStringLiteral("dataset")   };
    if (type == QLatin1StringView("image"))      return { QStringLiteral("IMAGE"),   QStringLiteral("dataset")   };
    if (type == QLatin1StringView("catalogue"))  return { QStringLiteral("CAT"),     QStringLiteral("catalogue") };
    if (type == QLatin1StringView("vbt"))        return { QStringLiteral("VBT"),     QStringLiteral("vbt")       };
    if (type == QLatin1StringView("vbt-volume")) return { QStringLiteral("VBT-VOL"), QStringLiteral("vbt")       };
    if (type == QLatin1StringView("hips"))       return { QStringLiteral("HiPS"),    QStringLiteral("hips")      };
    if (type == QLatin1StringView("velocity"))   return { QStringLiteral("VEL"),     QStringLiteral("dataset")   };
    return { type.toUpper(), QStringLiteral("dataset") };
}

}  // namespace

QFrame *DataHubWidget::buildRecentDatasetsPanel()
{
    auto *card = VisivoTheme::makeCard(this);

    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(20, 16, 20, 16);
    vbox->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->addWidget(VisivoTheme::makeSectionLabel(u"Recent Datasets"_s, card));
    header->addStretch(1);

    // Clearing the list was only possible by editing the settings file. The
    // button is hidden while the list is already empty rather than shown
    // disabled, so it never invites a click that would do nothing.
    m_clearRecentsButton = new QPushButton(u"Clear"_s, card);
    m_clearRecentsButton->setCursor(Qt::PointingHandCursor);
    m_clearRecentsButton->setFlat(true);
    // Sized and toned to the 10 px section heading it sits beside: at the
    // global button size it dominated the header, and showing or hiding it
    // moved the header's height.
    m_clearRecentsButton->setMinimumHeight(18);
    {
        // The header must not change height when the button appears or goes:
        // a hidden widget normally stops taking part in the layout.
        QSizePolicy sp = m_clearRecentsButton->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        m_clearRecentsButton->setSizePolicy(sp);
    }
    m_clearRecentsButton->setStyleSheet(
            QStringLiteral("QPushButton { color: %1; background: transparent; "
                           "border: none; font-size: 10px; padding: 0 2px; }"
                           "QPushButton:hover { color: %2; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                         QLatin1StringView(VisivoTheme::kOnSurface())));
    m_clearRecentsButton->setToolTip(
            u"Forget every dataset in this list. The files themselves are not "
            u"touched — only VisIVO's record of having opened them."_s);
    m_clearRecentsButton->hide();
    QObject::connect(m_clearRecentsButton, &QPushButton::clicked, this, [this]() {
        if (!this->recentMgr) return;
        const int count = this->recentMgr->datasets().size();
        if (count <= 0) return;
        const auto answer = QMessageBox::question(
                this, u"Clear Recent Datasets"_s,
                QStringLiteral("Forget %1 recent dataset%2?\n\n"
                               "This only clears the list. Nothing on disk is deleted.")
                        .arg(count)
                        .arg(count == 1 ? QString() : u"s"_s),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        this->recentMgr->clear();   // emits changed() -> refreshRecentDatasets()
    });
    header->addWidget(m_clearRecentsButton);
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


void DataHubWidget::setRecentDatasetsManager(RecentDatasetsManager *manager)
{
    this->recentMgr = manager;
    if (manager) {
        QObject::connect(manager, &RecentDatasetsManager::changed,
                         this, &DataHubWidget::refreshRecentDatasets);
    }
    refreshRecentDatasets();
}


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

    // Nothing to clear when there is nothing listed.
    if (this->m_clearRecentsButton) {
        this->m_clearRecentsButton->setVisible(!datasets.isEmpty());
    }

    if (datasets.isEmpty()) {
        // Empty-state label.
        auto *empty = new QLabel(
                u"No recently opened datasets.\n"
                u"Open one from Quick actions, beside this list."_s,
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

        // Filename row — name on the left, optional origin badge ("SKAVA")
        // on the right. The badge is shown for datasets opened via the
        // discovery + visivo-backend routing, so the operator can
        // distinguish them from plain local-path opens at a glance.
        auto *nameRowLayout = new QHBoxLayout;
        nameRowLayout->setContentsMargins(0, 0, 0, 0);
        nameRowLayout->setSpacing(6);

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
        // Elided, not truncated by the layout: a long path used to set this
        // card's minimum width and push the badge and the remove button past
        // the right edge, where the horizontal scrollbar is switched off and
        // they could not be reached at all. The full name stays in the
        // tooltip.
        nameLbl->setMinimumWidth(1);
        nameLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        ElideOnResize::apply(nameLbl, ds.displayName);
        // The tooltip goes on the ROW, not on this label: the label is
        // transparent to mouse events, so a tooltip set here is never shown.
        // displayName is not always a basename — the manager falls back to the
        // full id — so the untruncated name belongs somewhere reachable.
        QString tip = ds.source == u"skava"_s && !ds.obsId.isEmpty()
                ? u"%1\nSKAVA obs_id: %2"_s.arg(ds.displayName, ds.obsId)
                : u"%1\n%2"_s.arg(ds.displayName, ds.id);
        // Where it came from and who served it, when the entry recorded that.
        if (!ds.skavaUrl.isEmpty())
            tip += u"\nCatalogue: %1"_s.arg(ds.skavaUrl);
        if (!ds.servedBy.isEmpty())
            tip += u"\nServed by node %1"_s.arg(ds.servedBy);
        row->setToolTip(tip);
        nameRowLayout->addWidget(nameLbl, 1);

        if (ds.source == u"skava"_s) {
            auto *skavaBadge = new QLabel(u"SKAVA"_s, row);
            QFont bf(QStringLiteral("Lato"));
            bf.setPixelSize(9);
            bf.setWeight(QFont::Bold);
            bf.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
            skavaBadge->setFont(bf);
            skavaBadge->setStyleSheet(
                    QStringLiteral(
                        "color: white; background: #2c7be5; border-radius: 3px; "
                        "padding: 1px 5px; margin: 0;"));
            skavaBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
            skavaBadge->setToolTip(
                    u"Opened via SKAVA discovery routing, reopen will "
                    u"re-query SKAVA and route to the current backend."_s);
            nameRowLayout->addWidget(skavaBadge, 0, Qt::AlignVCenter);
        }
        nameRowLayout->addStretch();
        textCol->addLayout(nameRowLayout);

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

        // "Remove from recents" button. The button is opaque to mouse events,
        // so clicking it does NOT bubble up to the row's CardClickFilter and
        // does not trigger a reopen. Only the entry is removed — the actual
        // dataset on the backend / disk is untouched (we don't own it).
        auto *removeBtn = new QPushButton(u"✕"_s, row);
        removeBtn->setCursor(Qt::PointingHandCursor);
        removeBtn->setToolTip(u"Remove from Recent Datasets (does not delete the file)."_s);
        removeBtn->setFixedSize(22, 22);
        removeBtn->setFocusPolicy(Qt::NoFocus);
        removeBtn->setStyleSheet(QStringLiteral(
                "QPushButton {"
                " border: 1px solid transparent; border-radius: 11px;"
                " background: transparent; color: %1;"
                " font-size: 12px; padding: 0; margin: 0;"
                "}"
                "QPushButton:hover {"
                " background: rgba(232,80,80,0.18);"
                " border-color: rgba(232,80,80,0.55);"
                " color: #E85050;"
                "}")
                .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
        const QString dsId          = ds.id;
        const QString dsDisplayName = ds.displayName;
        QPointer<DataHubWidget> guard(this);
        QObject::connect(removeBtn, &QPushButton::clicked, this,
                         [guard, dsId, dsDisplayName]() {
                             if (!guard || !guard->recentMgr) return;
                             const auto reply = QMessageBox::question(
                                     guard, u"Remove from Recent Datasets"_s,
                                     u"Remove '%1' from the Recent Datasets list?\n\n"
                                     u"The actual dataset on the backend is not deleted."_s
                                             .arg(dsDisplayName));
                             if (reply != QMessageBox::Yes) return;
                             guard->recentMgr->remove(dsId);
                             // changed() signal triggers refreshRecentDatasets
                             // automatically, so the row disappears without
                             // an explicit refresh call here.
                         });
        hbox->addWidget(removeBtn, 0, Qt::AlignVCenter);

        // Click → emit reopenDatasetRequested.
        RecentDataset dsCopy = ds; // capture by value
        auto *filter = new datahub::CardClickFilter(
                [this, dsCopy]() { emit reopenDatasetRequested(dsCopy); }, row);
        row->installEventFilter(filter);

        // Insert before the trailing stretch.
        const int insertPos = this->recentItemsLayout->count() - 1;
        this->recentItemsLayout->insertWidget(insertPos, row);
    }
}
