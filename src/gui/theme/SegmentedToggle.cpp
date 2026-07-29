#include "SegmentedToggle.h"
#include "VisivoTheme.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QToolButton>

using namespace Qt::StringLiterals;

SegmentedToggle::SegmentedToggle(QWidget *parent)
    : QWidget(parent)
{
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(3, 3, 3, 3);     // inset: the chip floats in the track (§3.0c)
    m_layout->setSpacing(0);                       // segments are joined

    applyStyle();
    setAttribute(Qt::WA_StyledBackground, true);

    QObject::connect(m_group, &QButtonGroup::idClicked, this, [this](int idx) {
        emit segmentClicked(idx);   // fires even when re-clicking the current segment
        if (idx == m_currentIndex) return;
        m_currentIndex = idx;
        refreshSegmentLooks();
        emit currentIndexChanged(idx);
    });
}

int SegmentedToggle::count() const
{
    return m_group ? static_cast<int>(m_group->buttons().size()) : 0;
}

void SegmentedToggle::addItem(const QString &label, const QString &toolTip)
{
    auto *btn = new QToolButton(this);
    btn->setText(label);
    btn->setCheckable(true);
    btn->setAutoExclusive(false);   // exclusivity handled by QButtonGroup
    btn->setFocusPolicy(Qt::TabFocus);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // Segments share the container width equally (flex) so a narrow dock never
    // clips the last label ("MinIP"); the widget fills, it does not overflow.
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Segments share surplus width equally (flex), but never shrink below their
    // own label — an over-tight setMaximumWidth() on the container must overflow
    // visibly, not elide ("Moment Map" → "Mom…Map").
    btn->setMinimumWidth(btn->fontMetrics().horizontalAdvance(label) + 18);  // 8 px padding × 2 + 2
    if (!toolTip.isEmpty()) {
        btn->setToolTip(toolTip);
    }

    const int idx = count();
    m_group->addButton(btn, idx);
    m_layout->addWidget(btn);

    if (m_currentIndex < 0) {
        m_currentIndex = 0;
        btn->setChecked(true);
    }
    refreshSegmentLooks();
}

void SegmentedToggle::addItem(const QIcon &icon, const QString &toolTip)
{
    auto *btn = new QToolButton(this);
    btn->setIcon(icon);
    btn->setIconSize(QSize(14, 14));
    btn->setCheckable(true);
    btn->setAutoExclusive(false);
    btn->setFocusPolicy(Qt::TabFocus);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setMinimumWidth(30);   // icon-only: room for the glyph + padding
    btn->setToolTip(toolTip);   // mandatory — the glyph alone doesn't name the layout

    const int idx = count();
    m_group->addButton(btn, idx);
    m_layout->addWidget(btn);
    if (m_currentIndex < 0) {
        m_currentIndex = 0;
        btn->setChecked(true);
    }
    refreshSegmentLooks();
}

void SegmentedToggle::setItems(const QStringList &labels)
{
    // Clear existing.
    const auto buttons = m_group->buttons();
    for (auto *b : buttons) {
        m_group->removeButton(b);
        m_layout->removeWidget(b);
        b->deleteLater();
    }
    m_currentIndex = -1;
    for (const QString &l : labels) {
        addItem(l);
    }
}

void SegmentedToggle::setItemToolTip(int index, const QString &toolTip)
{
    if (auto *btn = m_group->button(index)) {
        btn->setToolTip(toolTip);
    }
}

void SegmentedToggle::setItemIcon(int index, const QIcon &icon)
{
    if (auto *btn = qobject_cast<QToolButton *>(m_group->button(index)))
        btn->setIcon(icon);
}

void SegmentedToggle::setCurrentIndex(int index)
{
    if (index < 0 || index >= count() || index == m_currentIndex) {
        // Still refresh paint state in case checkmarks drifted.
        if (auto *btn = m_group->button(index)) {
            const QSignalBlocker block(btn);
            btn->setChecked(true);
        }
        refreshSegmentLooks();
        return;
    }
    auto *btn = m_group->button(index);
    if (!btn) return;
    m_currentIndex = index;
    const QSignalBlocker block(btn);
    btn->setChecked(true);
    refreshSegmentLooks();
    emit currentIndexChanged(index);
}

void SegmentedToggle::applyStyle()
{
    // Container: 1px outline + 4px radius — matches the design tokens
    // (t.line border, borderRadius 4). Per-segment look set in
    // refreshSegmentLooks() so selected / unselected can use different
    // backgrounds + font weights without restyling the whole widget.
    // An INSET track (§3.0c): the container is a recessed rail (kSurface) with a
    // 5px radius; the selected chip sits inside it with a 4px radius and 3px of
    // padding around — the single change that reads most as "finished".
    const QString css = QStringLiteral(
        "SegmentedToggle {"
        "  background: %2;"
        "  border: 1px solid %1;"
        "  border-radius: 5px;"
        "}"
    ).arg(QString::fromLatin1(VisivoTheme::kOutline()),
          QString::fromLatin1(VisivoTheme::kSurface()));
    setStyleSheet(css);
}

void SegmentedToggle::refreshSegmentLooks()
{
    const QString primary    = QString::fromLatin1(VisivoTheme::kPrimary());
    const QString onPrimary  = QString::fromLatin1(VisivoTheme::kOnPrimary());
    const QString onVariant  = QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
    const QString primaryDim = QString::fromLatin1(VisivoTheme::kPrimaryDim());
    const QString surfaceHi  = QString::fromLatin1(VisivoTheme::kSurfaceContainerHigh());

    // Per-segment stylesheet template. Selected segment is filled with the
    // primary brand colour (#048ABF) + bold ink; unselected segments are
    // transparent with muted ink and a subtle hover lift.
    const QString sel = QStringLiteral(
        "QToolButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "}"
        "QToolButton:hover { background: %3; }"
    ).arg(primary, onPrimary, primaryDim);

    const QString unsel = QStringLiteral(
        "QToolButton {"
        "  background: transparent;"
        "  color: %1;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "  font-size: 11px;"
        "  font-weight: 400;"
        "}"
        "QToolButton:hover { background: %2; color: %3; }"
    ).arg(onVariant, surfaceHi, QString::fromLatin1(VisivoTheme::kOnSurface()));

    const auto buttons = m_group->buttons();
    for (int i = 0; i < buttons.size(); ++i) {
        auto *btn = buttons.at(i);
        btn->setStyleSheet(i == m_currentIndex ? sel : unsel);
    }
}
