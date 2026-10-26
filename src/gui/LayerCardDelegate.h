#ifndef LayerCardDelegate_h
#define LayerCardDelegate_h

// LayerCardDelegate.h
// One card per image layer, instead of one line of text.
//
// The layer list used to show a checkbox and a filename. Everything else about
// a layer — which colour map it carries, which stretch, how transparent it is —
// lived in the panel above the list and described *the selected layer only*, so
// with a Stokes set or half a dozen VLKB cutouts the only way to find the one
// you meant was to click each in turn and watch the panel change.
//
// The card puts those three on the row itself: a gradient strip of the layer's
// own LUT, its stretch, its opacity. That is the redesign's Screen G.1.2, and
// it is a delegate rather than a widget per row so the existing model, the
// drag-to-reorder (which is the z-order) and the selection all keep working
// untouched — only the painting changes.

#include <QStyledItemDelegate>

class LayerListModel;

class LayerCardDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit LayerCardDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /// Clicks on the visibility box toggle it. Without this the box is painted
    /// by us and therefore not hit-tested by anything.
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option, const QModelIndex &index) override;

private:
    /// The 14×14 box at the left of the row, in `option.rect` coordinates.
    static QRect checkboxRect(const QStyleOptionViewItem &option);
    static const LayerListModel *layerModel(const QModelIndex &index);
};

class QAbstractItemView;

namespace visivo {
/// Re-lay @p view out (once, queued) whenever its viewport's WIDTH changes,
/// so a delegate whose size hint is the viewport width never leaves cards
/// wider than the list — on a dock resize or when a scrollbar appears.
void relayoutOnViewportWidthChange(QAbstractItemView *view);
}

#endif   // LayerCardDelegate_h
