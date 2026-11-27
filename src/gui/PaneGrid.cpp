#include "PaneGrid.h"

#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

PaneGrid::PaneGrid(QSplitter *mainSplitter, QWidget *window, QObject *parent)
    : QObject(parent)
    , m_main(mainSplitter)
    , m_window(window)
{
}

PaneGrid::~PaneGrid()
{
    if (m_pressActivates)
        qApp->removeEventFilter(this);
}

void PaneGrid::build(bool installColumns)
{
    if (!m_main || m_slots[0].frame)
        return;
    const QLatin1StringView fg(VisivoTheme::kOnSurface());
    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView pri(VisivoTheme::kPrimary());

    for (int i = 0; i < visivo::panes::kSlots; ++i) {
        auto *frame = new QFrame();
        frame->setObjectName(u"paneFrame"_s);
        auto *v = new QVBoxLayout(frame);
        v->setContentsMargins(1, 1, 1, 1);
        v->setSpacing(0);

        auto *hdr = new QWidget(frame);
        hdr->setObjectName(u"paneHeader"_s);
        hdr->setFixedHeight(34);
        auto *h = new QHBoxLayout(hdr);
        h->setContentsMargins(10, 0, 6, 0);
        h->setSpacing(8);

        auto *dot = new QLabel(u"●"_s, hdr);
        dot->setStyleSheet(QStringLiteral("font-size:9px; color:%1;").arg(pri));
        h->addWidget(dot);

        auto *title = new QToolButton(hdr);   // plain text at 1 pane, dropdown at more
        title->setToolButtonStyle(Qt::ToolButtonTextOnly);
        title->setPopupMode(QToolButton::InstantPopup);
        title->setCursor(Qt::PointingHandCursor);
        title->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:none;color:%1;font-family:'Lato';"
                "font-size:11px;font-weight:700;} QToolButton::menu-indicator{image:none;}")
                                     .arg(fg));
        h->addWidget(title);

        auto *qual = new QLabel(QString(), hdr);
        qual->setStyleSheet(QStringLiteral("color:%1; font-family:'JetBrains Mono','SF Mono',"
                                           "monospace; font-size:10px;").arg(fgMuted));
        h->addWidget(qual);

        // Per-view control slot (the view's own buttons get re-hosted here).
        auto *ctrlHost = new QWidget(hdr);
        auto *ctrl = new QHBoxLayout(ctrlHost);
        ctrl->setContentsMargins(0, 0, 0, 0);
        ctrl->setSpacing(6);
        h->addWidget(ctrlHost);

        h->addStretch(1);

        auto *maxBtn = new QToolButton(hdr);
        maxBtn->setText(u"⤢"_s);
        maxBtn->setCursor(Qt::PointingHandCursor);
        maxBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        maxBtn->setToolTip(u"Maximize pane"_s);
        maxBtn->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;"
                                             "color:%1;font-size:13px;}QToolButton:hover{color:%2;}")
                                      .arg(fgMuted, pri));
        // The window decides what a maximise means for its views.
        QObject::connect(maxBtn, &QToolButton::clicked, this,
                         [this, i]() { emit maximiseToggled(i); });
        h->addWidget(maxBtn);

        // Pop-out: detach this pane into its own window (for a second screen). Re-attach with
        // the same button, the window close, or the placeholder's Bring back.
        auto *detachBtn = new QToolButton(hdr);
        detachBtn->setText(u"⤴"_s);
        detachBtn->setCursor(Qt::PointingHandCursor);
        detachBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        detachBtn->setToolTip(u"Pop out to a window"_s);
        detachBtn->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;"
                                                "color:%1;font-size:13px;}QToolButton:hover{color:%2;}")
                                         .arg(fgMuted, pri));
        QObject::connect(detachBtn, &QToolButton::clicked, this, [this, i]() {
            if (isDetached(i))
                dock(i);
            else
                detach(i);
        });
        h->addWidget(detachBtn);

        v->addWidget(hdr);

        auto *viewHost = new QWidget(frame);
        auto *vhl = new QHBoxLayout(viewHost);
        vhl->setContentsMargins(0, 0, 0, 0);
        vhl->setSpacing(0);
        v->addWidget(viewHost, 1);

        hdr->installEventFilter(this);

        Slot &s = m_slots[i];
        s.frame = frame;
        s.header = hdr;
        s.dot = dot;
        s.titleBtn = title;
        s.qualifier = qual;
        s.viewHost = viewHost;
        s.viewHostLayout = vhl;
        s.controls = ctrl;
        s.maxBtn = maxBtn;
        s.detachBtn = detachBtn;
    }

    if (installColumns)
        this->installColumns();
}

void PaneGrid::installColumns()
{
    if (!m_main || m_left || !m_slots[0].frame)
        return;
    // Nested columns: left = slot0/slot2, right = slot1/slot3.
    m_left = new QSplitter(Qt::Vertical);
    m_left->setChildrenCollapsible(false);
    m_left->addWidget(m_slots[0].frame);
    m_left->addWidget(m_slots[2].frame);
    m_left->setStretchFactor(0, 1);
    m_left->setStretchFactor(1, 1);
    m_right = new QSplitter(Qt::Vertical);
    m_right->setChildrenCollapsible(false);
    m_right->addWidget(m_slots[1].frame);
    m_right->addWidget(m_slots[3].frame);
    m_right->setStretchFactor(0, 1);
    m_right->setStretchFactor(1, 1);
    m_main->addWidget(m_left);
    m_main->addWidget(m_right);
    m_main->setStretchFactor(0, 1);
    m_main->setStretchFactor(1, 1);
}

void PaneGrid::addHeaderWidget(int slot, QWidget *w)
{
    if (slot < 0 || slot >= visivo::panes::kSlots || !w || !m_slots[slot].header)
        return;
    auto *h = qobject_cast<QHBoxLayout *>(m_slots[slot].header->layout());
    if (!h)
        return;
    const int at = m_slots[slot].maxBtn ? h->indexOf(m_slots[slot].maxBtn) : -1;
    w->setParent(m_slots[slot].header);
    h->insertWidget(at >= 0 ? at : h->count(), w);
}

QWidget *PaneGrid::makeDetachPlaceholder(int slot)
{
    // Occupies the grid slot while the pane lives in its own window — SAME chrome as a pane
    // (paneFrame border + dark body + a header bar with the title) so it reads as the
    // slot's own cell, not naked text. The caller copies the frame's size behaviour so
    // the splitter never re-flows.
    const QLatin1StringView ink(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView fg(VisivoTheme::kOnSurface());
    auto *ph = new QFrame();
    ph->setObjectName(u"paneFrame"_s);
    ph->setStyleSheet(u"QFrame#paneFrame{border:1px solid #1E2427;}"_s);
    auto *v = new QVBoxLayout(ph);
    v->setContentsMargins(1, 1, 1, 1);
    v->setSpacing(0);

    // Header bar (matches a pane header's height + dot + title).
    auto *hdr = new QWidget(ph);
    hdr->setObjectName(u"paneHeader"_s);
    hdr->setFixedHeight(34);
    auto *h = new QHBoxLayout(hdr);
    h->setContentsMargins(10, 0, 6, 0);
    h->setSpacing(8);
    auto *dot = new QLabel(u"○"_s, hdr);
    dot->setStyleSheet(u"font-size:9px; color:#3A4247;"_s);
    h->addWidget(dot);
    auto *title = new QLabel(QStringLiteral("Pane %1 · detached").arg(slot + 1), hdr);
    title->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;"
                                        "font-weight:700;").arg(fg));
    h->addWidget(title);
    h->addStretch(1);
    v->addWidget(hdr);

    // Body: dark canvas + centred Bring back.
    auto *body = new QWidget(ph);
    body->setStyleSheet(u"background:#000004;"_s);
    auto *bv = new QVBoxLayout(body);
    bv->addStretch(1);
    auto *sub = new QLabel(u"Open in its own window"_s, body);
    sub->setAlignment(Qt::AlignCenter);
    sub->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;").arg(ink));
    bv->addWidget(sub);
    auto *row = new QHBoxLayout();
    row->addStretch(1);
    auto *btn = new QPushButton(u"Bring back"_s, body);
    btn->setCursor(Qt::PointingHandCursor);
    QObject::connect(btn, &QPushButton::clicked, this, [this, slot]() { dock(slot); });
    row->addWidget(btn);
    row->addStretch(1);
    bv->addSpacing(8);
    bv->addLayout(row);
    bv->addStretch(1);
    v->addWidget(body, 1);
    return ph;
}

bool PaneGrid::detach(int slot)
{
    // Refused while out already, and while maximised: maximise + detach don't compose.
    if (!m_state.canDetach(slot) || !m_slots[slot].frame)
        return false;
    QSplitter *col = columnOf(slot);
    if (!col)
        return false;
    const int idx = col->indexOf(m_slots[slot].frame);
    if (idx < 0)
        return false;

    // Snapshot BOTH splitters' pixel sizes — QSplitter re-negotiates when a widget is
    // swapped, so re-apply them right after the swap.
    const QList<int> colSizes = col->sizes();
    const QList<int> mainSizes = m_main ? m_main->sizes() : QList<int>{};

    // The placeholder must occupy EXACTLY the frame's cell: same sizePolicy + minimumSize +
    // stretch so the splitter gives it the frame's extent, nothing more.
    QWidget *ph = makeDetachPlaceholder(slot);
    ph->setSizePolicy(m_slots[slot].frame->sizePolicy());
    ph->setMinimumSize(m_slots[slot].frame->minimumSize());
    ph->setMaximumSize(m_slots[slot].frame->maximumSize());
    m_slots[slot].placeholder = ph;
    QWidget *taken = col->replaceWidget(idx, ph);   // == m_slots[slot].frame
    col->setStretchFactor(idx, 1);
    if (col->count() == colSizes.size())
        col->setSizes(colSizes);
    if (m_main && !mainSizes.isEmpty() && m_main->count() == mainSizes.size())
        m_main->setSizes(mainSizes);

    // The frame (header + view, WITH its active border) moves to a bare top-level window —
    // no toolbar, no menu; the docks stay in the main window. A child of the owner window
    // (Qt::Window), so it is destroyed with it (no orphan on close).
    auto *win = new QWidget(m_window, Qt::Window);
    win->setObjectName(u"detachedPane"_s);
    win->setWindowTitle(m_title ? m_title(slot) : QStringLiteral("Pane %1").arg(slot + 1));
    win->setAttribute(Qt::WA_DeleteOnClose, false);
    auto *wl = new QVBoxLayout(win);
    wl->setContentsMargins(0, 0, 0, 0);
    if (taken)
        wl->addWidget(taken);
    if (m_slots[slot].frame)
        m_slots[slot].frame->show();
    m_slots[slot].detachedWindow = win;
    m_state.detach(slot);
    win->installEventFilter(this);   // Close → dock
    win->resize(640, 560);
    // Reopen where the user last left this slot's window (session memory).
    if (!m_slots[slot].detachedGeometry.isEmpty())
        win->restoreGeometry(m_slots[slot].detachedGeometry);
    win->show();
    win->raise();

    if (m_slots[slot].detachBtn) {
        m_slots[slot].detachBtn->setText(u"⤵"_s);
        m_slots[slot].detachBtn->setToolTip(u"Dock back into the grid"_s);
    }
    emit paneDetached(slot);
    return true;
}

bool PaneGrid::dock(int slot)
{
    if (!m_state.isDetached(slot))
        return false;
    QSplitter *col = columnOf(slot);
    if (!col)
        return false;

    // Snapshot both splitters' sizes so the dock is pixel-identical to before the detach.
    const QList<int> colSizes = col->sizes();
    const QList<int> mainSizes = m_main ? m_main->sizes() : QList<int>{};
    const int idx = m_slots[slot].placeholder ? col->indexOf(m_slots[slot].placeholder) : -1;
    if (m_slots[slot].frame && idx >= 0) {
        QWidget *oldPh = col->replaceWidget(idx, m_slots[slot].frame);
        col->setStretchFactor(idx, 1);
        if (oldPh)
            oldPh->deleteLater();
        m_slots[slot].placeholder = nullptr;
    } else if (m_slots[slot].frame) {
        col->addWidget(m_slots[slot].frame);   // fallback: append
        if (m_slots[slot].placeholder) {
            m_slots[slot].placeholder->setParent(nullptr);
            m_slots[slot].placeholder->deleteLater();
            m_slots[slot].placeholder = nullptr;
        }
    }
    if (col->count() == colSizes.size())
        col->setSizes(colSizes);
    if (m_main && !mainSizes.isEmpty() && m_main->count() == mainSizes.size())
        m_main->setSizes(mainSizes);
    QWidget *win = m_slots[slot].detachedWindow;
    m_slots[slot].detachedWindow = nullptr;   // null BEFORE deleting → dock is idempotent
    m_state.dock(slot);
    if (win) {
        m_slots[slot].detachedGeometry = win->saveGeometry();   // remember for a re-detach
        win->removeEventFilter(this);
        win->hide();
        win->deleteLater();
    }
    if (m_slots[slot].detachBtn) {
        m_slots[slot].detachBtn->setText(u"⤴"_s);
        m_slots[slot].detachBtn->setToolTip(u"Pop out to a window"_s);
    }
    emit paneDocked(slot);
    return true;
}

void PaneGrid::showOnly(int slot)
{
    if (slot < 0 || slot >= visivo::panes::kSlots || !m_slots[slot].frame || !m_left || !m_right)
        return;
    // Show ONLY this slot's frame + its column; the sibling column collapses so the
    // QSplitter gives the survivor the full width, and hiding the sibling frame gives
    // it the full height. No view is parked or re-rendered — pure visibility toggle.
    for (int i = 0; i < visivo::panes::kSlots; ++i)
        if (m_slots[i].frame)
            m_slots[i].frame->setVisible(i == slot);
    const bool inLeft = visivo::panes::inLeftColumn(slot);
    m_left->setVisible(inLeft);
    m_right->setVisible(!inLeft);
    if (m_slots[slot].maxBtn) {
        m_slots[slot].maxBtn->setText(u"⤡"_s);
        m_slots[slot].maxBtn->setToolTip(u"Restore layout"_s);
    }
    m_state.maximise();
}

void PaneGrid::resetMaximiseButtons()
{
    for (auto &s : m_slots)
        if (s.maxBtn) {
            s.maxBtn->setText(u"⤢"_s);
            s.maxBtn->setToolTip(u"Maximize pane"_s);
        }
    m_state.restore();
}

void PaneGrid::applyVisibility(const std::array<bool, visivo::panes::kSlots> &visible)
{
    for (int i = 0; i < visivo::panes::kSlots; ++i)
        if (m_slots[i].frame)
            m_slots[i].frame->setVisible(visible[i]);
    if (m_left)
        m_left->setVisible(visible[0] || visible[2]);
    if (m_right)
        m_right->setVisible(visible[1] || visible[3]);
}

void PaneGrid::equaliseSplits(int panes, bool stacked)
{
    // Equal ratio values suffice; QSplitter scales them to the real pixel extent.
    constexpr int kEq = 1 << 20;
    const QList<int> half{ kEq, kEq };
    if (panes == 4) {
        if (m_main)
            m_main->setSizes(half);
        if (m_left)
            m_left->setSizes(half);
        if (m_right)
            m_right->setSizes(half);
    } else if (panes == 2) {
        if (stacked) {
            if (m_left)
                m_left->setSizes(half);
        } else if (m_main) {
            m_main->setSizes(half);
        }
    }
}

void PaneGrid::styleSlot(int slot, bool active, bool armed)
{
    if (slot < 0 || slot >= visivo::panes::kSlots)
        return;
    // The active-pane distinction must be VISIBLE: brand-blue border + a filled dot; every
    // other pane gets a near-flush border and a dim dot, so "controls act here" reads
    // straight off the pane.
    static constexpr QLatin1StringView kInactiveBorder("#1E2427");
    static constexpr QLatin1StringView kInactiveDot("#3A4247");
    const QLatin1StringView pri(VisivoTheme::kPrimary());
    const QLatin1StringView warn(VisivoTheme::kWarning());
    Slot &s = m_slots[slot];
    if (s.frame)
        s.frame->setStyleSheet(
                armed ? QStringLiteral("QFrame#paneFrame { border: 2px solid %1; }").arg(warn)
                      : QStringLiteral("QFrame#paneFrame { border: 1px solid %1; }")
                                .arg(active ? pri : kInactiveBorder));
    if (s.dot)
        s.dot->setStyleSheet(
                QStringLiteral("font-size:9px; color:%1;").arg(active ? pri : kInactiveDot));
    // The active pane's header row carries an 8% primary wash.
    if (s.header)
        s.header->setStyleSheet(
                active ? QStringLiteral("QWidget#paneHeader { background: rgba(4,138,191,0.08); }")
                       : QStringLiteral("QWidget#paneHeader { background: transparent; }"));
}

void PaneGrid::setPressActivates(bool on)
{
    if (on == m_pressActivates)
        return;
    m_pressActivates = on;
    if (on)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
}

bool PaneGrid::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type et = event->type();
    if (m_pressActivates && et == QEvent::MouseButtonPress && watched->isWidgetType()) {
        const auto *w = static_cast<const QWidget *>(watched);
        for (int i = 0; i < visivo::panes::kSlots; ++i)
            if (m_slots[i].frame && m_slots[i].frame->isAncestorOf(w)) {
                emit headerPressed(i);
                break;
            }
        return false;
    }
    // Closing a popped-out window docks the pane into its grid slot (never destroys it).
    if (et == QEvent::Close) {
        for (int i = 0; i < visivo::panes::kSlots; ++i)
            if (m_slots[i].detachedWindow && watched == m_slots[i].detachedWindow) {
                dock(i);
                return true;   // handled: the frame was reparented out first
            }
    }
    if (et == QEvent::MouseButtonPress && !m_pressActivates) {
        for (int i = 0; i < visivo::panes::kSlots; ++i)
            if (m_slots[i].header && watched == m_slots[i].header) {
                emit headerPressed(i);
                break;
            }
    }
    // Double-click a pane header toggles pop-out.
    if (et == QEvent::MouseButtonDblClick) {
        for (int i = 0; i < visivo::panes::kSlots; ++i)
            if (m_slots[i].header && watched == m_slots[i].header) {
                if (isDetached(i))
                    dock(i);
                else
                    detach(i);
                return true;
            }
    }
    return QObject::eventFilter(watched, event);
}
