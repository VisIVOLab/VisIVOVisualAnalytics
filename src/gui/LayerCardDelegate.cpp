// LayerCardDelegate.cpp — see LayerCardDelegate.h.

#include "LayerCardDelegate.h"

#include "ColormapIcon.h"
#include "LayerCardGeometry.h"
#include "LayerListModel.h"
#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>

using namespace Qt::Literals::StringLiterals;

namespace {

// The metrics and the layout arithmetic live in LayerCardGeometry.h, where
// they are tested; these are the two names used often enough to alias here.
constexpr visivo::LayerCardMetrics kMetrics{};
constexpr int kPadX = kMetrics.paddingX;
constexpr int kGap = kMetrics.gap;
constexpr int kBoxSide = kMetrics.boxSide;
constexpr int kStripHeight = kMetrics.stripHeight;

/// One gradient per colormap, rendered once at a fixed width and scaled to the
/// strip. Building a 256-sample vtkLookupTable inside paint() would do it on
/// every repaint of every row; keying the cache by width as well would grow it
/// by one entry per pixel while the dock is being dragged.
QPixmap gradientFor(const std::string &name)
{
    static constexpr int kSourceWidth = 256;
    static QHash<QString, QPixmap> cache;
    const QString key = QString::fromStdString(name);
    auto found = cache.constFind(key);
    if (found != cache.constEnd())
        return found.value();
    const QPixmap pixmap =
            makeColormapIcon(name, kSourceWidth, kMetrics.stripHeight)
                    .pixmap(kSourceWidth, kMetrics.stripHeight);
    cache.insert(key, pixmap);
    return pixmap;
}

}   // namespace

LayerCardDelegate::LayerCardDelegate(QObject *parent) : QStyledItemDelegate(parent) { }

const LayerListModel *LayerCardDelegate::layerModel(const QModelIndex &index)
{
    return qobject_cast<const LayerListModel *>(index.model());
}

QRect LayerCardDelegate::checkboxRect(const QStyleOptionViewItem &option)
{
    return visivo::layerCheckboxRect(option.rect, kMetrics);
}

QSize LayerCardDelegate::sizeHint(const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    Q_UNUSED(index);
    // Deliberately not the model's SizeHintRole: the card is taller than the
    // one-line row it replaces, and the model's 36 px describes that old row.
    return QSize(option.rect.width(), kMetrics.height);
}

void LayerCardDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    const auto *model = layerModel(index);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool selected = option.state & QStyle::State_Selected;
    const QRect card = option.rect.adjusted(4, 2, -4, -2);

    // The selected layer is the one the settings above the list act on, so it
    // is the one that has to be unmistakable.
    const QColor fill = selected ? QColor(4, 138, 191, 41)
                                 : QColor(QLatin1StringView(VisivoTheme::kSurfaceContainerLow()));
    const QColor border = selected ? QColor(4, 138, 191, 102) : QColor(4, 138, 191, 36);
    painter->setPen(QPen(border, 1));
    painter->setBrush(fill);
    painter->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

    // ── Row 1: visibility, name, tag ─────────────────────────────────────────
    const QRect box = checkboxRect(option);
    const bool visible =
            index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
    // The item-view indicator, drawn through the VIEW's style and with the view
    // passed along: the theme styles `QAbstractItemView::indicator`, and a bare
    // QApplication::style() call with no widget context silently falls back to
    // the platform checkbox instead.
    QStyleOptionViewItem check(option);
    check.rect = box;
    check.state = (option.state & ~QStyle::State_HasFocus)
            | (visible ? QStyle::State_On : QStyle::State_Off);
    const QWidget *view = option.widget;
    (view ? view->style() : QApplication::style())
            ->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &check, painter, view);

    const int textLeft = box.right() + kGap;
    const int textRight = card.right() - kPadX;

    QString tag;
    if (model && index.row() == model->getMasterIndex())
        tag = u"base"_s;
    int tagWidth = 0;
    QFont monoFont = option.font;
    monoFont.setFamilies({ u"JetBrains Mono"_s, u"SF Mono"_s, u"Menlo"_s, u"monospace"_s });
    monoFont.setPixelSize(9);
    if (!tag.isEmpty()) {
        const QFontMetrics monoMetrics(monoFont);
        tagWidth = monoMetrics.horizontalAdvance(tag) + kGap;
        painter->setFont(monoFont);
        painter->setPen(QColor(QLatin1StringView(VisivoTheme::kPrimary())));
        painter->drawText(QRect(textRight - tagWidth + kGap, box.top(), tagWidth, kBoxSide),
                          Qt::AlignRight | Qt::AlignVCenter, tag);
    }

    QFont nameFont = option.font;
    nameFont.setPixelSize(12);
    painter->setFont(nameFont);
    painter->setPen(QColor(QLatin1StringView(VisivoTheme::kOnSurface())));
    const QRect nameRect(textLeft, box.top(), textRight - textLeft - tagWidth, kBoxSide);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideMiddle, nameRect.width()));

    // ── Row 2: the layer's own colour map, stretch and opacity ───────────────
    if (!model) {
        painter->restore();
        return;
    }
    const int row = index.row();
    const QString scale =
            QString::fromLatin1(LutScale::modeLabels().at(
                                        static_cast<std::size_t>(model->scaleMode(row))))
                    .toLower();
    const QString opacity = u"%1%"_s.arg(std::lround(model->getLayerOpacity(row) * 100.0));

    painter->setFont(monoFont);
    const QFontMetrics monoMetrics(monoFont);
    const QString detail = u"%1 · %2"_s.arg(scale, opacity);
    const int detailWidth = monoMetrics.horizontalAdvance(detail) + kGap;

    const QRect strip = visivo::layerStripRect(card, detailWidth, kMetrics);
    const int stripTop = strip.top();
    painter->drawPixmap(strip, gradientFor(model->getColorMapName(row)));
    // A hairline around the strip, or a dark colormap dissolves into the card.
    painter->setPen(QPen(QColor(4, 138, 191, 60), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(strip);

    painter->setPen(QColor(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    painter->drawText(QRect(strip.right() + kGap, stripTop - 1, detailWidth, kStripHeight + 2),
                      Qt::AlignRight | Qt::AlignVCenter, detail);

    // A hidden layer is still listed, but it should read as switched off.
    if (!visible) {
        painter->setBrush(QColor(QLatin1StringView(VisivoTheme::kSurface())));
        painter->setPen(Qt::NoPen);
        painter->setOpacity(0.45);
        painter->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }

    painter->restore();
}

bool LayerCardDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                    const QStyleOptionViewItem &option, const QModelIndex &index)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease: {
        // Mouse events are answered HERE and nowhere else. The base class
        // hit-tests a checkbox of its own — vertically centred in a row that is
        // now 52 px tall — so forwarding a click that missed our box would let
        // it toggle visibility from an empty-looking part of the card.
        const QRect hit = visivo::layerVisibilityHitRect(option.rect, kMetrics);
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton || !hit.contains(mouse->pos()))
            return false;   // not ours: the view selects and starts drags as usual
        if (event->type() != QEvent::MouseButtonRelease)
            return true;    // handled: the toggle happens on the release
        const bool visible =
                index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
        model->setData(index, visible ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
        return true;
    }
    default:
        // Keyboard toggling (Space) and everything else stays with the base.
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
}
