#include "IsoLegend.h"

#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMargin = 12;   // from the view's right edge
constexpr int kTop = 44;      // clear of the view's header controls
constexpr int kPad = 8;
constexpr int kSwatch = 12;
constexpr int kGap = 6;
}

IsoLegend::IsoLegend(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSizeF(10.0);
    setFont(f);
    hide();
}

void IsoLegend::attachTo(QWidget *host)
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

void IsoLegend::setLevels(const QVector<visivo::isolevels::Level> &levels, const QString &unit)
{
    m_levels = levels;
    m_unit = unit;
    reposition();
    update();
}

QString IsoLegend::rowText(const visivo::isolevels::Level &level, const QString &unit)
{
    return QStringLiteral("%1   %2 %")
            .arg(visivo::isolevels::label(level.value, unit))
            .arg(static_cast<int>(std::lround(level.opacity * 100.0)));
}

bool IsoLegend::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        reposition();
    return QWidget::eventFilter(obj, event);
}

void IsoLegend::reposition()
{
    QWidget *p = parentWidget();
    if (!p)
        return;
    const QFontMetrics fm(font());
    int text = fm.horizontalAdvance(QStringLiteral("Isosurfaces"));
    for (const auto &level : m_levels)
        text = std::max(text, kSwatch + kGap + fm.horizontalAdvance(rowText(level, m_unit)));
    const int w = kPad + text + kPad;
    const int h = kPad + fm.height() + 4 + static_cast<int>(m_levels.size()) * (fm.height() + 2)
            + kPad;
    setGeometry(p->width() - w - kMargin, kTop, w, h);
    raise();   // above the render surface
}

void IsoLegend::paintEvent(QPaintEvent *)
{
    if (m_levels.isEmpty())
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm(font());
    const QColor textColour(235, 235, 235);
    const QColor halo(0, 0, 0, 190);
    const auto label = [&painter, textColour, halo](int x, int y, const QString &text) {
        painter.setPen(halo);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx != 0 || dy != 0)
                    painter.drawText(x + dx, y + dy, text);
        painter.setPen(textColour);
        painter.drawText(x, y, text);
    };
    int y = kPad + fm.ascent();
    label(kPad, y, QStringLiteral("Isosurfaces"));
    y += fm.height() + 4;
    // Highest level first: the inner surface of an emission peak is the top row.
    for (int i = static_cast<int>(m_levels.size()) - 1; i >= 0; --i) {
        const auto &level = m_levels[i];
        const int top = y - fm.ascent() + (fm.height() - kSwatch) / 2;
        painter.setPen(QPen(halo, 3.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(kPad, top, kSwatch, kSwatch);
        painter.setPen(QColor(255, 255, 255, 160));
        painter.setBrush(level.colour);
        painter.drawRect(kPad, top, kSwatch, kSwatch);
        label(kPad + kSwatch + kGap, y, rowText(level, m_unit));
        y += fm.height() + 2;
    }
}
