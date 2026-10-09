#include "DockRail.h"

#include <QEvent>
#include <QFont>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

using namespace Qt::StringLiterals;

namespace {

QColor tokenColor(const char *token)
{
    return QColor(QLatin1StringView(token));
}

} // namespace

DockRail::DockRail(const QString &title, const QString &expandGlyph, QWidget *parent)
    : QWidget(parent), m_title(title), m_glyph(expandGlyph)
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background:%1;").arg(QLatin1StringView(VisivoTheme::kSurface())));
    setFixedWidth(VisivoTheme::kRailWidth);
}

void DockRail::setEntries(const QList<Entry> &entries)
{
    m_entries = entries;
    update();
}

void DockRail::setActiveEntry(int index)
{
    if (index == m_active)
        return;
    m_active = index;
    update();
}

void DockRail::setCount(int count)
{
    if (count == m_count)
        return;
    m_count = count;
    update();
}

void DockRail::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int W = VisivoTheme::kRailWidth;
    const int H = height();
    const int cx = W / 2;

    // 1. expand glyph (upright, kPrimary), 36 × 28 at the top — the affordance.
    m_glyphRect = QRect(0, 4, W, 28);
    {
        QFont gf = font();
        gf.setPixelSize(13);
        p.setFont(gf);
        p.setPen(tokenColor(VisivoTheme::kPrimary()));
        p.drawText(m_glyphRect, Qt::AlignCenter, m_glyph);
    }

    // 2. a 20 × 1 px rule, centred.
    p.setPen(tokenColor(VisivoTheme::kOutlineVariant()));
    p.drawLine(cx - 10, 36, cx + 10, 36);

    // 3. entries as STACKED ICONS (no rotated text — illegible in a 36 px rail; the
    //    name is the tooltip). 28 px cells; active one tinted kPrimary on a soft
    //    pill, the rest kOnSurfaceVariant.
    m_entryRects.clear();
    const int cell = 28;   // button
    const int gap = 6;     // step = 34 — a tight group, not a scattered list (§3.0c)
    const int iconSz = 18;
    int y = 44;
    for (int i = 0; i < m_entries.size(); ++i) {
        const QRect cellRect(0, y, W, cell);
        m_entryRects.push_back(cellRect);
        const bool active = (i == m_active);
        if (active) {
            QColor pill = tokenColor(VisivoTheme::kPrimary());
            pill.setAlphaF(0.16f);
            p.setPen(Qt::NoPen);
            p.setBrush(pill);
            p.drawRoundedRect(QRect(4, y + 2, W - 8, cell - 4), 4, 4);
        }
        const QColor tint = tokenColor(active ? VisivoTheme::kPrimary()
                                              : VisivoTheme::kOnSurfaceVariant());
        const QPixmap px = VisivoTheme::renderThemeIcon(m_entries[i].iconPath, iconSz, &tint,
                                                        this->devicePixelRatioF());
        if (!px.isNull()) {
            const int ix = cx - iconSz / 2;
            const int iy = y + (cell - iconSz) / 2;
            p.drawPixmap(QRect(ix, iy, iconSz, iconSz), px);
        }
        y += cell + gap;
    }
    // No count is drawn — it read as an orphan "0" (§3.0c); the running-task and
    // product counts live in the status rail.
}

void DockRail::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->pos();
    if (m_glyphRect.contains(pos)) {
        emit expandRequested();
        return;
    }
    for (int i = 0; i < m_entryRects.size(); ++i) {
        if (m_entryRects[i].contains(pos)) {
            emit entryClicked(i);
            return;
        }
    }
    // A click anywhere else on the rail also expands — the whole strip is an
    // affordance, and the rotated glyph is a small target.
    emit expandRequested();
}

bool DockRail::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(e);
        QString tip = QStringLiteral("Expand %1").arg(m_title);
        for (int i = 0; i < m_entryRects.size(); ++i) {
            if (m_entryRects[i].contains(help->pos())) {
                tip = m_entries[i].tooltip;
                break;
            }
        }
        QToolTip::showText(help->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(e);
}
