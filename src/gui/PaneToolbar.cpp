#include "PaneToolbar.h"

#include "PaneLayoutModel.h"
#include "theme/SegmentedToggle.h"
#include "theme/VisivoTheme.h"

#include <QColor>
#include <QHBoxLayout>
#include <QIcon>
#include <QSignalBlocker>
#include <QToolButton>

using namespace Qt::StringLiterals;

PaneToolbar::PaneToolbar(QWidget *parent)
    : QWidget(parent)
{
    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView pri(VisivoTheme::kPrimary());

    setObjectName(u"paneToolbar"_s);
    setFixedHeight(30);
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(8, 0, 8, 0);
    m_layout->setSpacing(8);

    m_countToggle = new SegmentedToggle(this);
    const QColor glyph{ QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()) };
    m_countToggle->addItem(VisivoTheme::renderThemeIcon(u":/icons/pane_1.svg"_s, 14, &glyph),
                           u"One pane"_s);
    m_countToggle->addItem(VisivoTheme::renderThemeIcon(u":/icons/pane_2.svg"_s, 14, &glyph),
                           u"Two panes"_s);
    m_countToggle->addItem(VisivoTheme::renderThemeIcon(u":/icons/pane_4.svg"_s, 14, &glyph),
                           u"Four panes"_s);
    m_countToggle->setCurrentIndex(0);   // spec §3.1 default: 1 pane
    m_countToggle->setMaximumWidth(108);
    connect(m_countToggle, &SegmentedToggle::currentIndexChanged, this,
            [this](int i) { emit paneCountRequested(i == 0 ? 1 : (i == 1 ? 2 : 4)); });
    // A second click on the "2" glyph flips the split direction (an explicit override of
    // the width heuristic, which is the default, not a cage).
    connect(m_countToggle, &SegmentedToggle::segmentClicked, this, [this](int idx) {
        if (idx == 1 && m_panes == 2)
            emit splitFlipRequested();
    });
    m_layout->addWidget(m_countToggle);

    m_linked = new QToolButton(this);
    m_linked->setText(u" Linked"_s);
    m_linked->setCheckable(true);
    m_linked->setCursor(Qt::PointingHandCursor);
    m_linked->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_linked->setIcon(QIcon(VisivoTheme::renderThemeIcon(u":/icons/rail_viz.svg"_s, 13, &glyph)));
    m_linked->setStyleSheet(
            QStringLiteral("QToolButton{background:transparent;border:1px solid %1;border-radius:5px;"
                           "color:%2;font-size:11px;padding:2px 8px;}"
                           "QToolButton:checked{color:%3;border-color:%3;}"
                           "QToolButton:disabled{color:%2;}")
                    .arg(QLatin1StringView(VisivoTheme::kOutline()), fgMuted, pri));
    m_layout->addWidget(m_linked);
    m_layout->addStretch(1);
}

void PaneToolbar::addWindowWidget(QWidget *w)
{
    if (!w)
        return;
    m_layout->insertWidget(1 + m_windowWidgets, w);
    ++m_windowWidgets;
}

QToolButton *PaneToolbar::addChip(const Chip &chip)
{
    const QLatin1StringView fgMuted(VisivoTheme::kOnSurfaceVariant());
    const QLatin1StringView pri(VisivoTheme::kPrimary());

    auto *c = new QToolButton(this);
    c->setText(chip.text);
    c->setToolTip(chip.toolTip);
    c->setCheckable(true);
    c->setChecked(chip.checked);
    c->setCursor(Qt::PointingHandCursor);
    c->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // Pill affordance: a bordered chip that clearly reads as a toggle. Off = muted text +
    // faint outline; on = accent text/border + a faint tint; hover brightens the outline;
    // disabled (Linked off / nothing to link) greys out.
    c->setStyleSheet(QStringLiteral(
            "QToolButton{background:transparent;border:1px solid %1;border-radius:4px;"
            "font-family:'JetBrains Mono','SF Mono',monospace;font-size:10px;"
            "padding:1px 6px;color:%2;}"
            "QToolButton:checked{color:%3;border-color:%3;background:rgba(120,170,187,0.14);}"
            "QToolButton:hover:enabled{border-color:%3;}"
            "QToolButton:disabled{color:#3A4247;border-color:#2A3237;background:transparent;}")
                             .arg(QLatin1StringView(VisivoTheme::kOutline()), fgMuted, pri));
    if (chip.onToggled)
        connect(c, &QToolButton::toggled, this, chip.onToggled);
    // After Linked and the chips before it, before the stretch; each chip is followed by
    // a 4 px gap (on top of the layout's own spacing).
    const int at = 2 + m_windowWidgets + m_chipItems;
    m_layout->insertWidget(at, c);
    m_layout->insertSpacing(at + 1, 4);
    m_chipItems += 2;
    m_chips.append(c);
    return c;
}

void PaneToolbar::setReadoutWidget(QWidget *w)
{
    if (!w)
        return;
    w->setParent(this);
    w->show();
    m_layout->addWidget(w);
}

void PaneToolbar::setLayoutState(int panes, bool stacked)
{
    m_panes = visivo::panes::normalisedCount(panes);
    const QSignalBlocker block(m_countToggle);
    m_countToggle->setCurrentIndex(m_panes == 1 ? 0 : (m_panes == 2 ? 1 : 2));
    // The "2" glyph shows the current split direction (two rows vs two columns).
    const QColor glyph{ QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()) };
    m_countToggle->setItemIcon(
            1, VisivoTheme::renderThemeIcon(
                       stacked ? u":/icons/pane_2_rows.svg"_s : u":/icons/pane_2.svg"_s, 14, &glyph));
}

void PaneToolbar::setLinkAvailable(bool hasTargets, bool linkedOn)
{
    m_linked->setEnabled(hasTargets);
    const bool chips = visivo::panes::linkChipsEnabled(hasTargets, linkedOn);
    for (QToolButton *c : std::as_const(m_chips))
        c->setEnabled(chips);
}

void PaneToolbar::setChipChecked(QToolButton *chip, bool checked)
{
    if (!chip)
        return;
    const QSignalBlocker block(chip);
    chip->setChecked(checked);
}

int PaneToolbar::itemCount() const
{
    return m_layout->count();
}
