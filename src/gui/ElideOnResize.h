#ifndef ElideOnResize_h
#define ElideOnResize_h

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QObject>
#include <QPointer>
#include <QString>

// ---------------------------------------------------------------------------
// ElideOnResize — keep a label's text elided to the width the layout gives it.
//
// A label with a long, unelided string reports that string's width as the
// width it needs, and every layout above it inherits that. Inside a scroll
// area whose horizontal scrollbar is switched off — which is how the Data Hub
// panels are built — the result is not a scrollbar but silent overflow: one
// long filename in one row widens the whole container and pushes the buttons
// at the right end of *every* row off the edge, where they cannot be reached
// at all.
//
// Eliding to a constant width does not fix it: how much room the name has
// depends on the thumbnail, the badge, the timestamp and the buttons beside
// it, and on the window. So the full string is stored on the label as a
// property and re-elided whenever the label is resized or its font changes —
// always from the original, never from an already-elided copy.
//
// The caller is expected to let the label shrink, which the size hint alone
// will not allow:
//
//     label->setMinimumWidth(1);
//     label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
//     ElideOnResize::apply(label, fullText);
//
// The untruncated text belongs somewhere reachable — usually a tooltip on the
// row, since these labels are often transparent to mouse events and a tooltip
// set on them would never be shown. Note that on a selectable label a selection
// copies what is displayed, so a name long enough to be elided copies with its
// ellipsis; the tooltip is the authoritative full string.
// ---------------------------------------------------------------------------

class ElideOnResize : public QObject
{
public:
    /// Idempotent: applying it twice to the same label updates the text
    /// rather than installing a second filter.
    static void apply(QLabel *label, const QString &fullText,
                      Qt::TextElideMode mode = Qt::ElideMiddle)
    {
        if (!label) return;
        label->setProperty("visivoFullText", fullText);
        label->setProperty("visivoElideMode", static_cast<int>(mode));
        // A property flag rather than findChild: this class deliberately has
        // no Q_OBJECT (it is header-only and needs no signals), and findChild
        // requires one.
        if (!label->property("visivoElideInstalled").toBool()) {
            label->setProperty("visivoElideInstalled", true);
            new ElideOnResize(label);
        }
        elide(label);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_label
            && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange)) {
            elide(m_label);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    explicit ElideOnResize(QLabel *label)
        : QObject(label)
        , m_label(label)
    {
        label->installEventFilter(this);
    }

    static void elide(QLabel *label)
    {
        if (!label) return;
        const QString full = label->property("visivoFullText").toString();
        if (full.isEmpty()) {
            // Applied with an empty string: clear, rather than leave whatever
            // the label happened to show.
            if (!label->text().isEmpty())
                label->clear();
            return;
        }
        const int available = label->width();
        if (available <= 0) return;
        const auto mode = static_cast<Qt::TextElideMode>(
                label->property("visivoElideMode").toInt());
        const QFontMetrics fm(label->font());
        const QString shown = fm.elidedText(full, mode, available);
        if (shown != label->text())
            label->setText(shown);
    }

    QPointer<QLabel> m_label;
};

#endif // ElideOnResize_h
