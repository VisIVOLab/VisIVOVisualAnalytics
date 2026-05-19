#pragma once

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

    /// Replace all segments at once (clears the selection).
    void setItems(const QStringList &labels);

    /// Set a tooltip on a specific segment.
    void setItemToolTip(int index, const QString &toolTip);

    /// Select segment `index`. Out-of-range values are clamped/ignored.
    /// Emits `currentIndexChanged` when the selection actually changes.
    void setCurrentIndex(int index);

    /// Current selection (-1 when no items are present).
    int currentIndex() const { return m_currentIndex; }

    int count() const;

signals:
    void currentIndexChanged(int index);

private:
    void applyStyle();
    void refreshSegmentLooks();

    QHBoxLayout   *m_layout      { nullptr };
    QButtonGroup  *m_group       { nullptr };
    int            m_currentIndex{ -1 };
};
