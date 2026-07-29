#pragma once

#include <QIcon>
#include <QStringList>
#include <QWidget>

class QAbstractButton;
class QButtonGroup;
class QHBoxLayout;

/// Compact segmented toggle ("pill") — N short labels joined inside a single
/// rounded border, the selected one filled with the brand primary colour.
/// Matches the "Observatory console" design token set:
///
///     ┌────────────────────────────────┐
///     │ Volume │ Iso │ MIP │ MinIP │   │
///     └────────────────────────────────┘
///
/// Container: 1px outline border, 4px radius, 1px inner padding.
/// Segment:   3×8px padding, 11px font, fontWeight 600 when selected.
///
/// The widget exposes a `QComboBox`-flavoured API (`addItem`, `setCurrentIndex`,
/// `currentIndex`, `currentIndexChanged(int)`) so it can replace existing
/// combo-based mode selectors with minimal changes at call sites.
class SegmentedToggle : public QWidget
{
    Q_OBJECT

public:
    explicit SegmentedToggle(QWidget *parent = nullptr);

    /// Append a segment with the given label.
    void addItem(const QString &label, const QString &toolTip = {});

    /// Append an icon-only segment (e.g. the 1/2/4 pane glyphs). The tooltip is
    /// mandatory — a bare glyph doesn't say which layout it is (§3.1).
    void addItem(const QIcon &icon, const QString &toolTip);

    /// Replace all segments at once (clears the selection).
    void setItems(const QStringList &labels);

    /// Set a tooltip on a specific segment.
    void setItemToolTip(int index, const QString &toolTip);

    /// Replace a segment's icon (e.g. rotate the "2" glyph rows↔cols).
    void setItemIcon(int index, const QIcon &icon);

    /// Select segment `index`. Out-of-range values are clamped/ignored.
    /// Emits `currentIndexChanged` when the selection actually changes.
    void setCurrentIndex(int index);

    /// Current selection (-1 when no items are present).
    int currentIndex() const { return m_currentIndex; }

    int count() const;

signals:
    void currentIndexChanged(int index);
    /// Fires on ANY segment click, including re-clicking the already-selected one
    /// (currentIndexChanged does not) — used for "second click inverts" behaviour.
    void segmentClicked(int index);

private:
    void applyStyle();
    void refreshSegmentLooks();

    QHBoxLayout   *m_layout      { nullptr };
    QButtonGroup  *m_group       { nullptr };
    int            m_currentIndex{ -1 };
};
