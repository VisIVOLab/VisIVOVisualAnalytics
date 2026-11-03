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
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSizeF(10.0);
    setFont(f);
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
    m_colours = colours;
    update();
}

void VolumeColorbar::setTicks(const QVector<QPair<double, QString>> &ticks)
{
    if (ticks == m_ticks)
        return;
    m_ticks = ticks;
    reposition();   // the width follows the longest label
    update();
}

void VolumeColorbar::setTitle(const QString &title)
{
    if (title == m_title)
        return;
    m_title = title;
    reposition();
    update();
}

void VolumeColorbar::setThreshold(std::optional<double> position, const QString &caption)
{
    const QString shown = position ? caption : QString();
    const bool resize = shown != m_thresholdCaption;
    m_threshold = position;
    m_thresholdCaption = shown;
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

void VolumeColorbar::reposition()
{
    QWidget *p = parentWidget();
    if (!p)
        return;
    const QFontMetrics fm(font());
    int labels = 0;
    for (const auto &tick : m_ticks)
        labels = std::max(labels, fm.horizontalAdvance(tick.second));
    const int w = std::max({ kPad + kBarWidth + kTickLength + 4 + labels + kPad,
                             kPad + fm.horizontalAdvance(m_title) + kPad,
                             kPad + fm.horizontalAdvance(m_thresholdCaption) + kPad });
    // From 10 % to 90 % of the view's height, as the former VBT viewer's bar
    // (vtkScalarBarActor at y 0.1, height 0.8), clear of the header controls.
    const int top = std::max(kTop, p->height() / 10);
    const int bottom = std::max(top + 120, p->height() * 9 / 10);
    const int h = std::min(bottom, p->height() - kMargin) - top;
    setGeometry(p->width() - w - kMargin, top, w, std::max(0, h));
    raise();   // above the render surface
}

void VolumeColorbar::paintEvent(QPaintEvent *)
{
    if (m_colours.isEmpty())
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath backdrop;
    backdrop.addRoundedRect(QRectF(rect()), 6, 6);
    painter.fillPath(backdrop, QColor(0, 0, 0, 140));

    const QFontMetrics fm(font());
    const int titleHeight = m_title.isEmpty() ? 0 : fm.height() + 4;
    const int captionHeight = m_thresholdCaption.isEmpty() ? 0 : fm.height() + 4;
    const int top = kPad + titleHeight + fm.ascent() / 2;
    const int bottom = height() - kPad - captionHeight - fm.ascent() / 2;
    const int span = std::max(1, bottom - top);
    const auto yAt = [&](double position) {
        return bottom - static_cast<int>(std::lround(std::clamp(position, 0.0, 1.0) * span));
    };

    // The bar, one row per pixel; under the threshold, dimmed.
    const int n = m_colours.size();
    for (int y = top; y <= bottom; ++y) {
        const double u = static_cast<double>(bottom - y) / span;
        QColor c = m_colours[std::clamp(static_cast<int>(std::lround(u * (n - 1))), 0, n - 1)];
        if (m_threshold && u < *m_threshold)
            c = QColor(c.red() / 4, c.green() / 4, c.blue() / 4);
        painter.fillRect(kPad, y, kBarWidth, 1, c);
    }
    painter.setPen(QColor(255, 255, 255, 90));
    painter.drawRect(kPad, top, kBarWidth, span);

    painter.setPen(QColor(235, 235, 235));
    if (!m_title.isEmpty())
        painter.drawText(kPad, kPad + fm.ascent(), m_title);
    if (!m_thresholdCaption.isEmpty())
        painter.drawText(kPad, height() - kPad - fm.descent(), m_thresholdCaption);
    const int tickX = kPad + kBarWidth;
    for (const auto &[position, text] : m_ticks) {
        const int y = yAt(position);
        painter.drawLine(tickX, y, tickX + kTickLength, y);
        painter.drawText(tickX + kTickLength + 4, y + fm.ascent() / 2 - 1, text);
    }

    if (m_threshold) {
        // The threshold across the bar, with a notch on its left.
        const int y = yAt(*m_threshold);
        painter.setPen(QPen(QColor(255, 255, 255), 1.5));
        painter.drawLine(kPad - 3, y, kPad + kBarWidth + 1, y);
        QPainterPath notch;
        notch.moveTo(kPad - 3, y - 3);
        notch.lineTo(kPad, y);
        notch.lineTo(kPad - 3, y + 3);
        notch.closeSubpath();
        painter.fillPath(notch, QColor(255, 255, 255));
    }
}
