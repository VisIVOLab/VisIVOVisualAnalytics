// SidebarPanel.cpp
// Shared collapsible sidebar component used by all main visualization windows.
// See SidebarPanel.h for the public API.

#include "SidebarPanel.h"
#include "theme/VisivoTheme.h"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QStyle>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

SidebarPanel::SidebarPanel(int expandedWidth, QWidget *parent)
    : QWidget(parent)
    , m_expandedWidth(expandedWidth)
{
    setObjectName(u"sidebarContainer"_s);

    // The panel starts expanded: rail (48 px) + content panel.
    const int fullWidth = kRailWidth + m_expandedWidth;
    setMinimumWidth(kRailWidth);
    setMaximumWidth(fullWidth);

    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setSpacing(0);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    // ── Rail ─────────────────────────────────────────────────────────────────
    m_rail = new QWidget(this);
    m_rail->setObjectName(u"sidebarRail"_s);
    m_rail->setFixedWidth(kRailWidth);

    m_railLayout = new QVBoxLayout(m_rail);
    m_railLayout->setSpacing(2);
    m_railLayout->setContentsMargins(4, 8, 4, 8);

    m_toggleBtn = new QToolButton(m_rail);
    m_toggleBtn->setCheckable(false);
    m_toggleBtn->setText(u"◀"_s);
    m_toggleBtn->setToolTip(u"Collapse controls sidebar"_s);
    m_toggleBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_toggleBtn->setFixedHeight(28);
    m_railLayout->addWidget(m_toggleBtn);

    // Spacer pushed to the end; section buttons are inserted before it.
    m_railLayout->addStretch(1);

    rootLayout->addWidget(m_rail);

    // ── Content panel ─────────────────────────────────────────────────────────
    // Each page inside sidebarStack is itself a QScrollArea, so:
    //   • sidebarStack fills whatever height the panel has
    //   • each page scrolls its own content independently
    //   • no content is clipped regardless of window height
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(u"sidebarStack"_s);
    rootLayout->addWidget(m_stack, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Page registration
// ─────────────────────────────────────────────────────────────────────────────

int SidebarPanel::addPage(const QString &iconResource, const QString &tooltip, QWidget *content)
{
    Q_ASSERT(!m_finalized);

    // Wrap each content widget in a per-page scroll area.
    // This means the QStackedWidget can fill the sidebar height exactly, and
    // each page independently scrolls its own content when the window is short.
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(content);

    const int index = m_stack->addWidget(scrollArea);

    // Rail button for this section. We render the SVG with `currentColor`
    // substituted by the active theme's muted-text hex via VisivoTheme so
    // the icons match the chrome (no more pre-baked light-blue strokes that
    // wash out on light theme).
    auto *btn = new QToolButton(m_rail);
    {
        QIcon icon;
        const QPixmap muted = VisivoTheme::renderThemeIcon(iconResource, 22);
        if (!muted.isNull()) {
            icon.addPixmap(muted, QIcon::Normal);
            // Active state: brand-blue tint when checked / on rail-button
            // [active='true'] property — keeps the rail's selected indicator
            // visually consistent with Layout B.
            const QColor primary(QString::fromLatin1(VisivoTheme::kPrimary()));
            const QPixmap active =
                    VisivoTheme::renderThemeIcon(iconResource, 22, &primary);
            if (!active.isNull()) {
                icon.addPixmap(active, QIcon::Active);
                icon.addPixmap(active, QIcon::Selected);
            }
            btn->setIcon(icon);
        } else {
            btn->setIcon(QIcon(iconResource));   // fallback
        }
    }
    btn->setIconSize(QSize(22, 22));
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn->setToolTip(tooltip);
    btn->setAutoRaise(true);
    // Force the button to fill the rail's usable width, otherwise QToolButton's
    // default size policy (Minimum × Fixed) lets QVBoxLayout centre a narrow
    // ~30 px button inside the 40 px content area — the icon then sits in the
    // button's geometry centre but the button itself is offset from the rail
    // edges, which reads as "icons drift left" relative to the active-state
    // 2 px brand bar that anchors the rail. Expanding horizontally fixes both.
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setFixedHeight(36);

    // Insert before the trailing stretch (which is at count()-1).
    m_railLayout->insertWidget(m_railLayout->count() - 1, btn);
    m_railBtns.append(btn);

    m_pending.append({ iconResource, tooltip, content });

    return index;
}

int SidebarPanel::addPage(const PageDef &def)
{
    return addPage(def.iconResource, def.tooltip, def.content);
}

// ─────────────────────────────────────────────────────────────────────────────
// Finalization
// ─────────────────────────────────────────────────────────────────────────────

void SidebarPanel::finalize(int initialPage)
{
    Q_ASSERT(!m_finalized);
    Q_ASSERT(!m_railBtns.isEmpty());
    m_finalized = true;

    applyStyles();

    // Wire toggle button.
    QObject::connect(m_toggleBtn, &QToolButton::clicked, this, &SidebarPanel::toggleExpanded);

    // Wire each rail button.
    for (int i = 0; i < m_railBtns.size(); ++i) {
        QToolButton *btn = m_railBtns[i];
        QObject::connect(btn, &QToolButton::clicked, this, [this, i, btn]() {
            setActiveButton(btn);
            switchToPage(i);
        });
    }

    // Show the initial page.
    if (initialPage >= 0 && initialPage < m_railBtns.size()) {
        m_stack->setCurrentIndex(initialPage);
        setActiveButton(m_railBtns[initialPage]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Styles
// ─────────────────────────────────────────────────────────────────────────────

void SidebarPanel::applyStyles()
{
    // Theme-aware colors. Containers track the surface palette so the sidebar
    // blends with the surrounding shell in either dark or light mode. Hover /
    // pressed / active accents derive from the brand blue alpha overlay so they
    // read on any background.
    const auto containerBg = QLatin1StringView(VisivoTheme::kSurfaceContainerLow());
    const auto railBg      = QLatin1StringView(VisivoTheme::kSurface());
    const auto hoverBg     = QLatin1StringView(VisivoTheme::kSurfaceContainerHigh());
    const auto pressedBg   = QLatin1StringView(VisivoTheme::kSurfaceContainerHighest());
    const auto activeBg    = QLatin1StringView(VisivoTheme::kSurfaceContainerHigh());
    const auto mutedText   = QLatin1StringView(VisivoTheme::kOnSurfaceVariant());

    // Container background.
    setStyleSheet(
            QStringLiteral("QWidget#sidebarContainer {"
                           "  background-color: %1;"
                           "  border-right: 1px solid rgba(4, 138, 191, 0.18);"
                           "}")
                    .arg(containerBg));

    // Rail.
    m_rail->setStyleSheet(
            QStringLiteral("QWidget#sidebarRail {"
                           "  background-color: %1;"
                           "  border-right: 1px solid rgba(4, 138, 191, 0.18);"
                           "}")
                    .arg(railBg));

    // Section buttons share one stylesheet; [active='true'] is flipped via
    // setProperty("active", …) + style()->polish() — no extra code paths needed.
    // Rail button stylesheet.
    //
    // Centering note: the active-state indicator is a 2px coloured border on
    // the left edge. To keep the icon optically centred at all times, we
    // reserve those 2px on every button via `border-left: 2px solid transparent`
    // and add a matching `padding-right: 2px`. Without this, only the active
    // button's icon would shift 2px right and the rail would look misaligned.
    const QString railBtnStyle =
            QStringLiteral("QToolButton {"
                           "  background-color: transparent;"
                           "  border-top: none;"
                           "  border-right: none;"
                           "  border-bottom: none;"
                           "  border-left: 2px solid transparent;"
                           "  border-radius: 6px;"
                           "  padding: 4px 2px 4px 0px;"
                           "}"
                           "QToolButton:hover   { background-color: %1; }"
                           "QToolButton:pressed { background-color: %2; }"
                           "QToolButton[active='true'] {"
                           "  background-color: %3;"
                           "  border-left: 2px solid #048ABF;"
                           "}")
                    .arg(hoverBg, pressedBg, activeBg);

    const QString toggleBtnStyle =
            QStringLiteral("QToolButton {"
                           "  background-color: transparent;"
                           "  color: %1;"
                           "  border: none;"
                           "  border-radius: 6px;"
                           "  font-size: 13px;"
                           "  padding: 4px 0px;"
                           "}"
                           "QToolButton:hover   { background-color: %2; color: #048ABF; }"
                           "QToolButton:pressed { background-color: %3; }")
                    .arg(mutedText, hoverBg, pressedBg);

    m_toggleBtn->setStyleSheet(toggleBtnStyle);
    for (QToolButton *btn : m_railBtns) {
        btn->setStyleSheet(railBtnStyle);
    }

    // Scroll areas: transparent background, no border.
    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto *sa = qobject_cast<QScrollArea *>(m_stack->widget(i))) {
            sa->setStyleSheet(
                    u"QScrollArea {"
                    "  background-color: transparent;"
                    "  border: none;"
                    "}"
                    "QScrollArea > QWidget > QWidget {"
                    "  background-color: transparent;"
                    "}"_s);
            if (sa->verticalScrollBar()) {
                sa->verticalScrollBar()->setStyleSheet(
                        u"QScrollBar:vertical {"
                        "  background: transparent;"
                        "  width: 6px;"
                        "  margin: 0;"
                        "}"
                        "QScrollBar::handle:vertical {"
                        "  background: rgba(4, 138, 191, 0.35);"
                        "  border-radius: 3px;"
                        "  min-height: 20px;"
                        "}"
                        "QScrollBar::add-line:vertical,"
                        "QScrollBar::sub-line:vertical { height: 0; }"_s);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public state
// ─────────────────────────────────────────────────────────────────────────────

bool SidebarPanel::isExpanded() const
{
    return m_expanded;
}

int SidebarPanel::currentPage() const
{
    return m_stack->currentIndex();
}

// ─────────────────────────────────────────────────────────────────────────────
// Toggle action for View menu
// ─────────────────────────────────────────────────────────────────────────────

QAction *SidebarPanel::createToggleAction(const QString &label, QObject *parent)
{
    auto *action = new QAction(label, parent ? parent : this);
    action->setCheckable(true);
    action->setChecked(true);
    action->setShortcut(QKeySequence(u"Ctrl+\\"_s));
    m_toggleAction = action;

    QObject::connect(action, &QAction::toggled, this, [this](bool checked) {
        setExpanded(checked);
    });
    QObject::connect(this, &SidebarPanel::expandedChanged, action, [action](bool expanded) {
        QSignalBlocker blocker(action);
        action->setChecked(expanded);
    });

    return action;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expand / collapse
// ─────────────────────────────────────────────────────────────────────────────

void SidebarPanel::setExpanded(bool expanded, bool animated)
{
    if (m_expanded == expanded) {
        return;
    }
    m_expanded = expanded;

    // Sync toggle button label/tooltip.
    m_toggleBtn->setText(expanded ? u"◀"_s : u"▶"_s);
    m_toggleBtn->setToolTip(expanded ? u"Collapse controls sidebar"_s
                                     : u"Expand controls sidebar"_s);

    const int toWidth = expanded ? (kRailWidth + m_expandedWidth) : kRailWidth;
    const int fromWidth = width();

    if (!animated) {
        setMinimumWidth(toWidth);
        setMaximumWidth(toWidth);
        m_stack->setVisible(expanded);
        if (expanded && m_pendingPage >= 0) {
            m_stack->setCurrentIndex(m_pendingPage);
            m_pendingPage = -1;
        }
        emit expandedChanged(expanded);
        return;
    }

    // Show the stack immediately when expanding so it grows with the animation.
    if (expanded) {
        m_stack->setVisible(true);
    }

    // Stop and delete any in-flight animation.
    if (m_animGroup) {
        m_animGroup->stop();
    }

    auto *animMin = new QPropertyAnimation(this, "minimumWidth");
    animMin->setDuration(kAnimDurationMs);
    animMin->setEasingCurve(QEasingCurve::InOutCubic);
    animMin->setStartValue(fromWidth);
    animMin->setEndValue(toWidth);

    auto *animMax = new QPropertyAnimation(this, "maximumWidth");
    animMax->setDuration(kAnimDurationMs);
    animMax->setEasingCurve(QEasingCurve::InOutCubic);
    animMax->setStartValue(fromWidth);
    animMax->setEndValue(toWidth);

    auto *group = new QParallelAnimationGroup(this);
    group->addAnimation(animMin);
    group->addAnimation(animMax);
    m_animGroup = group;

    QObject::connect(group, &QParallelAnimationGroup::finished, this, [this, expanded]() {
        if (!expanded) {
            m_stack->setVisible(false);
        } else if (m_pendingPage >= 0) {
            m_stack->setCurrentIndex(m_pendingPage);
            m_pendingPage = -1;
        }
        emit expandedChanged(expanded);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void SidebarPanel::toggleExpanded()
{
    setExpanded(!m_expanded);
}

// ─────────────────────────────────────────────────────────────────────────────
// Page switching
// ─────────────────────────────────────────────────────────────────────────────

void SidebarPanel::switchToPage(int index)
{
    if (m_expanded) {
        m_stack->setCurrentIndex(index);
        emit pageChanged(index);
    } else {
        // Defer the page switch until the expand animation finishes.
        m_pendingPage = index;
        setExpanded(true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Active rail button highlight
// ─────────────────────────────────────────────────────────────────────────────

void SidebarPanel::setActiveButton(QToolButton *btn)
{
    for (QToolButton *b : m_railBtns) {
        const bool active = (b == btn);
        if (b->property("active").toBool() != active) {
            b->setProperty("active", active);
            b->style()->polish(b);
        }
    }
}
