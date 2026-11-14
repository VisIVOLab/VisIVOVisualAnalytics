#include "HeadsetActivityList.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

HeadsetActivityList::HeadsetActivityList(QWidget *parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    // The rows wrap to the panel's width instead.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rows = new QWidget(this);
    m_rowsLayout = new QVBoxLayout(m_rows);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(6);
    m_rowsLayout->addStretch(1);
    setWidget(m_rows);
}

QString HeadsetActivityList::wrappable(const QString &text)
{
    return VrHub::wrappable(text);
}

void HeadsetActivityList::setEvents(const QList<VrHub::Event> &events)
{
    // Everything but the closing stretch goes. Hidden at once (out of the
    // layout), deleted later: a rebuild may run under an Open button's click.
    while (m_rowsLayout->count() > 1) {
        QLayoutItem *item = m_rowsLayout->takeAt(0);
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    const auto addRow = [this](QWidget *row) { m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, row); };
    if (events.isEmpty()) {
        auto *none = new QLabel(u"Nothing done in the headset yet."_s, m_rows);
        none->setWordWrap(true);
        addRow(none);
        return;
    }
    for (const VrHub::Event &e : events) {
        auto *row = new QWidget(m_rows);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(4, 2, 4, 2);
        auto *text = new QLabel(wrappable(VrHub::eventRowText(e)), row);
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setToolTip(e.cubePath.isEmpty() ? QString() : u"Cube: %1"_s.arg(e.cubePath));
        h->addWidget(text, 1);
        if (e.kind == u"product"_s) {
            const VrHub::Product p = VrHub::parseProduct(e);
            auto *open = new QToolButton(row);
            open->setText(u"Open"_s);
            // Its own size, at the top of a row that wraps onto more lines.
            open->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            const VrHub::OpenIn where = VrHub::openIn(p);
            open->setEnabled(where != VrHub::OpenIn::Nothing);
            open->setToolTip(where == VrHub::OpenIn::Nothing ? u"The headset sent no file for it."_s
                                                             : u"Open it here."_s);
            QObject::connect(open, &QToolButton::clicked, this, [this, e]() { emit openRequested(e); });
            h->addWidget(open, 0, Qt::AlignTop);
        }
        addRow(row);
    }
}
