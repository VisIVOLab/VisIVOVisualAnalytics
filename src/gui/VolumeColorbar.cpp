#include "VolumeColorbar.h"

#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMargin = 12;     // from the view's right edge
constexpr int kTop = 44;        // clear of the view's header controls
constexpr int kBarWidth = 12;
constexpr int kPad = 8;
constexpr int kTickLength = 4;
}

VolumeColorbar::VolumeColorbar(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setFont(barFont());
}

QFont VolumeColorbar::barFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSizeF(10.0);
    return f;
}

void VolumeColorbar::attachTo(QWidget *host)
{
    if (!host || host == parentWidget())
        return;
    if (QWidget *current = parentWidget())
        current->removeEventFilter(this);
    const bool shown = !isHidden();
    setParent(host);
    host->installEventFilter(this);
    setVisible(shown);   // setParent() hides a widget
    reposition();
}

void VolumeColorbar::setColours(const QVector<QColor> &colours)
{
    m_look.colours = colours;
    update();
}

void VolumeColorbar::setTicks(const QVector<QPair<double, QString>> &ticks)
{
    if (ticks == m_look.ticks)
        return;
    m_look.ticks = ticks;
    reposition();   // the width follows the longest label
    update();
}

void VolumeColorbar::setTitle(const QString &title)
{
    if (title == m_look.title)
        return;
    m_look.title = title;
    reposition();
    update();
}

void VolumeColorbar::setThreshold(std::optional<double> position, const QString &caption)
{
    const QString shown = position ? caption : QString();
    const bool resize = shown != m_look.thresholdCaption;
    m_look.threshold = position;
    m_look.thresholdCaption = shown;
    if (resize)
        reposition();
    update();
}

bool VolumeColorbar::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        reposition();
    return QWidget::eventFilter(obj, event);
}

QRect VolumeColorbar::placement(const QSize &host, const QFontMetrics &fm, const Look &look)
{
    int labels = 0;
    for (const auto &tick : look.ticks)
        labels = std::max(labels, fm.horizontalAdvance(tick.second));
    const int w = std::max({ kPad + kBarWidth + kTickLength + 4 + labels + kPad,
                             kPad + fm.horizontalAdvance(look.title) + kPad,
                             kPad + fm.horizontalAdvance(look.thresholdCaption) + kPad,
                             // Room for any threshold caption, so the bar does
                             // not shift as its value or the blend mode changes.
                             kPad + fm.horizontalAdvance(QStringLiteral("threshold -8.88e-88 < Min")) + kPad });
    // From 10 % to 90 % of the view's height, as the former VBT viewer's bar
    // (vtkScalarBarActor at y 0.1, height 0.8), clear of the header controls.
    const int top = std::max(kTop, host.height() / 10);
    const int bottom = std::max(top + 120, host.height() * 9 / 10);
    const int h = std::min(bottom, host.height() - kMargin) - top;
    return { host.width() - w - kMargin, top, w, std::max(0, h) };
}

void VolumeColorbar::reposition()
{
    QWidget *p = parentWidget();
    if (!p)
        return;
    setGeometry(placement(p->size(), QFontMetrics(font()), m_look));
    raise();   // above the render surface
}

void VolumeColorbar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    paint(painter, rect(), QFontMetrics(font()), m_look);
}

void VolumeColorbar::paint(QPainter &painter, const QRect &rect, const QFontMetrics &fm,
                           const Look &look)
{
    if (look.colours.isEmpty())
        return;
    painter.save();
    painter.translate(rect.topLeft());
    const int width = rect.width(), height = rect.height();
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath backdrop;
    backdrop.addRoundedRect(QRectF(0, 0, width, height), 6, 6);
    painter.fillPath(backdrop, QColor(0, 0, 0, 140));

    const int titleHeight = look.title.isEmpty() ? 0 : fm.height() + 4;
    const int captionHeight = look.thresholdCaption.isEmpty() ? 0 : fm.height() + 4;
    const int top = kPad + titleHeight + fm.ascent() / 2;
    const int bottom = height - kPad - captionHeight - fm.ascent() / 2;
    const int span = std::max(1, bottom - top);
    const auto yAt = [&](double position) {
        return bottom - static_cast<int>(std::lround(std::clamp(position, 0.0, 1.0) * span));
    };

    // The bar, one row per pixel; under the threshold, dimmed.
    const int n = look.colours.size();
    for (int y = top; y <= bottom; ++y) {
        const double u = static_cast<double>(bottom - y) / span;
        QColor c = look.colours[std::clamp(static_cast<int>(std::lround(u * (n - 1))), 0, n - 1)];
        if (look.threshold && u < *look.threshold)
            c = QColor(c.red() / 4, c.green() / 4, c.blue() / 4);
        painter.fillRect(kPad, y, kBarWidth, 1, c);
    }
    painter.setPen(QColor(255, 255, 255, 90));
    painter.drawRect(kPad, top, kBarWidth, span);

    painter.setPen(QColor(235, 235, 235));
    if (!look.title.isEmpty())
        painter.drawText(kPad, kPad + fm.ascent(), look.title);
    if (!look.thresholdCaption.isEmpty())
        painter.drawText(kPad, height - kPad - fm.descent(), look.thresholdCaption);
    const int tickX = kPad + kBarWidth;
    for (const auto &[position, text] : look.ticks) {
        const int y = yAt(position);
        painter.drawLine(tickX, y, tickX + kTickLength, y);
        painter.drawText(tickX + kTickLength + 4, y + fm.ascent() / 2 - 1, text);
    }

    if (look.threshold) {
        // The threshold across the bar, with a notch on its left.
        const int y = yAt(*look.threshold);
        painter.setPen(QPen(QColor(255, 255, 255), 1.5));
        painter.drawLine(kPad - 3, y, kPad + kBarWidth + 1, y);
        QPainterPath notch;
        notch.moveTo(kPad - 3, y - 3);
        notch.lineTo(kPad, y);
        notch.lineTo(kPad - 3, y + 3);
        notch.closeSubpath();
        painter.fillPath(notch, QColor(255, 255, 255));
    }
    painter.restore();
}
