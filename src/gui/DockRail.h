#ifndef DOCKRAIL_H
#define DOCKRAIL_H

// DockRail — the 36 px collapsed state of a workspace dock (Design 1d, §4.1).
// One widget for both sides: an expand glyph at the top, then the Inspector's tab
// names as stacked, tinted ICONS (the tab name is the tooltip — rotated text is
// illegible in 36 px, §3.0c). Painted directly (fixed layout). Never 0 px, never
// hidden — collapsing swaps to this.

#include "theme/VisivoTheme.h"

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>

class DockRail : public QWidget
{
    Q_OBJECT
public:
    explicit DockRail(const QString &title, const QString &expandGlyph, QWidget *parent = nullptr);

    /// A clickable rail entry: a themed icon (the tab's glyph) with the tab name in
    /// the tooltip. The collapsed rail is icons only — no rotated text (§3.0c).
    struct Entry
    {
        QString iconPath;   ///< Qt-resource SVG, tinted by renderThemeIcon
        QString tooltip;    ///< the tab name (rotated text is illegible in 36 px)
    };
    void setEntries(const QList<Entry> &entries);
    void setActiveEntry(int index);
    /// Kept for API compatibility; the count is no longer drawn (it read as an
    /// orphan "0" under the collapsed rail — the real counts live in the status
    /// rail, §3.0c).
    void setCount(int count);

signals:
    void expandRequested();
    void entryClicked(int index);   ///< expands the dock AND selects that tab

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    bool event(QEvent *) override;   ///< per-entry tooltips (rotated text is small)
    QSize sizeHint() const override { return { VisivoTheme::kRailWidth, 200 }; }

private:
    QString m_title;
    QString m_glyph;
    QList<Entry> m_entries;
    int m_active{ -1 };
    int m_count{ 0 };
    QRect m_glyphRect;
    QList<QRect> m_entryRects;   ///< filled in paintEvent, hit-tested in mousePress
};

#endif // DOCKRAIL_H
