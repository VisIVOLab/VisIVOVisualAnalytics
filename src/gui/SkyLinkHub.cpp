#include "SkyLinkHub.h"

#include "RegionSkyConvert.h"

#include <algorithm>

#include <QApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QInputDialog>
#include <QStringList>
#include <QTimer>
#include <QWidget>

using namespace Qt::StringLiterals;
using namespace visivo::skylink;

namespace {

/// Shown when a window has no WCS the link can place pixels with.
QString noSkyText(const SkyLinkEndpoint *e)
{
    return u"%1 has no sky coordinates the link can use: it needs RA/Dec or Galactic axes in a TAN, "
           u"SIN, ARC, STG or CAR projection."_s.arg(e->skyLinkLabel());
}

/// One of @p list, by the label of its window; null when the user cancels.
SkyLinkEndpoint *pickEndpoint(QWidget *parent, const QString &title, const QString &prompt,
                              const QList<SkyLinkEndpoint *> &list)
{
    if (list.size() == 1)
        return list.first();
    QStringList labels;
    for (SkyLinkEndpoint *e : list) {
        QString l = e->skyLinkLabel();
        int copy = 1;
        while (labels.contains(l))
            l = u"%1 [%2]"_s.arg(e->skyLinkLabel()).arg(++copy);
        labels.append(l);
    }
    bool ok = false;
    const QString picked = QInputDialog::getItem(parent, title, prompt, labels, 0, false, &ok);
    const int at = ok ? labels.indexOf(picked) : -1;
    return at >= 0 ? list.at(at) : nullptr;
}

void bringForward(SkyLinkEndpoint *e)
{
    if (QWidget *w = e ? e->skyLinkWidget() : nullptr) {
        if (w->isMinimized())
            w->showNormal();
        w->raise();
        w->activateWindow();
    }
}

}   // namespace

SkyLinkHub &SkyLinkHub::instance()
{
    static SkyLinkHub hub;
    return hub;
}

QList<SkyLinkEndpoint *> SkyLinkHub::candidates(const SkyLinkEndpoint *self) const
{
    QList<SkyLinkEndpoint *> out;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        auto *e = dynamic_cast<SkyLinkEndpoint *>(w);
        if (e && e != self && e->skyLinkIsCube() != self->skyLinkIsCube()
            && (w->isVisible() || w->isMinimized()))
            out.append(e);
    }
    return out;
}

void SkyLinkHub::prune()
{
    m_pairs.erase(std::remove_if(m_pairs.begin(), m_pairs.end(),
                                 [](const Pair &p) { return !p.a || !p.b; }),
                  m_pairs.end());
}

bool SkyLinkHub::isLinked(const SkyLinkEndpoint *self) const
{
    for (const Pair &p : m_pairs)
        if (p.a && p.b && (p.ea == self || p.eb == self))
            return true;
    return false;
}

QList<SkyLinkEndpoint *> SkyLinkHub::peers(const SkyLinkEndpoint *self) const
{
    QList<SkyLinkEndpoint *> out;
    for (const Pair &p : m_pairs) {
        if (!p.a || !p.b)
            continue;
        if (p.ea == self)
            out.append(p.eb);
        else if (p.eb == self)
            out.append(p.ea);
    }
    return out;
}

bool SkyLinkHub::link(SkyLinkEndpoint *a, SkyLinkEndpoint *b)
{
    if (!a || !b || a == b || a->skyLinkIsCube() == b->skyLinkIsCube())
        return false;
    // The headers are read here, once: a window that cannot say where its pixels
    // are on the sky cannot be linked, and the reason is better told now than
    // after a pan that does nothing.
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const bool okA = a->skyWcs() != nullptr;
    const bool okB = okA && b->skyWcs() != nullptr;
    QGuiApplication::restoreOverrideCursor();
    if (!okA || !okB) {
        a->skyMessage(noSkyText(okA ? b : a));
        return false;
    }
    prune();
    for (const Pair &p : m_pairs)
        if (p.a && p.b && ((p.ea == a && p.eb == b) || (p.ea == b && p.eb == a))) {
            syncFrom(a);
            return true;
        }
    m_pairs.append({ a->skyLinkWidget(), b->skyLinkWidget(), a, b });
    a->skyLinkStateChanged();
    b->skyLinkStateChanged();
    a->skyMessage(u"Linked to %1: pan, zoom and cursor follow in sky coordinates."_s.arg(b->skyLinkLabel()));
    b->skyMessage(u"Linked to %1: pan, zoom and cursor follow in sky coordinates."_s.arg(a->skyLinkLabel()));
    syncFrom(a);
    return true;
}

void SkyLinkHub::unlink(SkyLinkEndpoint *self)
{
    QList<SkyLinkEndpoint *> others;
    for (const Pair &p : std::as_const(m_pairs)) {
        if (!p.a || !p.b)
            continue;
        if (p.ea == self)
            others.append(p.eb);
        else if (p.eb == self)
            others.append(p.ea);
    }
    m_pairs.erase(std::remove_if(m_pairs.begin(), m_pairs.end(),
                                 [self](const Pair &p) { return p.ea == self || p.eb == self; }),
                  m_pairs.end());
    self->skyCursorHide();
    self->skyLinkStateChanged();
    for (SkyLinkEndpoint *o : std::as_const(others)) {
        o->skyCursorHide();
        o->skyLinkStateChanged();
        o->skyMessage(u"Sky link with %1 ended."_s.arg(self->skyLinkLabel()));
    }
    self->skyMessage(u"Sky link ended."_s);
}

void SkyLinkHub::forget(SkyLinkEndpoint *self)
{
    QList<SkyLinkEndpoint *> others;
    for (const Pair &p : std::as_const(m_pairs)) {
        if (p.ea == self && p.b)
            others.append(p.eb);
        else if (p.eb == self && p.a)
            others.append(p.ea);
    }
    m_pending.remove(self->skyLinkWidget());
    m_pairs.erase(std::remove_if(m_pairs.begin(), m_pairs.end(),
                                 [self](const Pair &p) { return p.ea == self || p.eb == self; }),
                  m_pairs.end());
    for (SkyLinkEndpoint *o : std::as_const(others)) {
        o->skyCursorHide();
        o->skyLinkStateChanged();
    }
}

SkyLinkEndpoint *SkyLinkHub::chooseTarget(SkyLinkEndpoint *from, const QString &title,
                                          const QString &prompt)
{
    prune();
    QList<SkyLinkEndpoint *> list = peers(from);
    if (list.isEmpty())
        list = candidates(from);
    if (list.isEmpty()) {
        from->skyMessage(from->skyLinkIsCube()
                                 ? u"No image window is open: open a 2-D image of the same sky first."_s
                                 : u"No cube window is open: open the cube first."_s);
        return nullptr;
    }
    SkyLinkEndpoint *picked = pickEndpoint(from->skyLinkWidget(), title, prompt, list);
    // The dialog ran its own event loop: the window picked may have closed.
    if (picked && !peers(from).contains(picked) && !candidates(from).contains(picked))
        return nullptr;
    return picked;
}

void SkyLinkHub::chooseAndLink(SkyLinkEndpoint *self)
{
    prune();
    const QList<SkyLinkEndpoint *> list = candidates(self);
    if (list.isEmpty()) {
        self->skyMessage(self->skyLinkIsCube()
                                 ? u"No image window is open: open a 2-D image of the same sky first."_s
                                 : u"No cube window is open: open the cube first."_s);
        return;
    }
    SkyLinkEndpoint *target = pickEndpoint(
            self->skyLinkWidget(), u"Link to"_s,
            self->skyLinkIsCube() ? u"Image window to link this cube to:"_s
                                  : u"Cube window to link this image to:"_s,
            list);
    // The dialog ran its own event loop: the window picked may have closed.
    if (!target || !candidates(self).contains(target))
        return;
    link(self, target);
}

void SkyLinkHub::syncFrom(SkyLinkEndpoint *from)
{
    const SkyWcs *fromWcs = from->skyWcs();
    View view;
    if (!fromWcs || !from->skyViewRead(view))
        return;
    for (SkyLinkEndpoint *p : peers(from)) {
        const SkyWcs *toWcs = p->skyWcs();
        View out;
        if (!toWcs)
            continue;
        if (!matchView(*fromWcs, view, *toWcs, out)) {
            from->skyMessage(u"%1 does not cover the sky this view is centred on."_s.arg(p->skyLinkLabel()));
            continue;
        }
        p->skyViewApply(out);
    }
}

void SkyLinkHub::notifyViewMoved(SkyLinkEndpoint *from)
{
    QWidget *w = from ? from->skyLinkWidget() : nullptr;
    if (!w || m_pending.contains(w))
        return;
    m_pending.insert(w);
    // Not from inside the frame that moved: a drag draws a frame per mouse
    // move and they join one update, and the peer does not draw while this
    // window is still finishing its own frame.
    QPointer<QWidget> guard(w);
    QTimer::singleShot(0, qApp, [this, guard, w]() {
        m_pending.remove(w);
        if (!guard)
            return;
        if (auto *e = dynamic_cast<SkyLinkEndpoint *>(guard.data()))
            syncFrom(e);
    });
}

void SkyLinkHub::cursorMoved(SkyLinkEndpoint *from, double px, double py)
{
    // A peer redraws for every cursor position: the mouse reports far more of
    // them than anyone can follow.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastCursorMs < 25)
        return;
    m_lastCursorMs = now;
    const SkyWcs *fromWcs = from->skyWcs();
    if (!fromWcs)
        return;
    for (SkyLinkEndpoint *p : peers(from)) {
        const SkyWcs *toWcs = p->skyWcs();
        double tx = 0.0, ty = 0.0;
        if (toWcs && matchPixel(*fromWcs, px, py, *toWcs, tx, ty))
            p->skyCursorShow(tx, ty);
        else
            p->skyCursorHide();
    }
}

void SkyLinkHub::sendRegions(SkyLinkEndpoint *from, bool selectedOnly)
{
    const auto regions = from->skyRegions(selectedOnly);
    if (regions.empty()) {
        from->skyMessage(selectedOnly ? u"Select a region first."_s : u"There are no regions to send."_s);
        return;
    }
    SkyLinkEndpoint *to = chooseTarget(
            from, u"Send Regions to"_s,
            from->skyLinkIsCube() ? u"Image window to send the regions to:"_s
                                  : u"Cube window to send the regions to:"_s);
    if (!to)
        return;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const SkyWcs *fromWcs = from->skyWcs();
    const SkyWcs *toWcs = fromWcs ? to->skyWcs() : nullptr;
    QGuiApplication::restoreOverrideCursor();
    if (!fromWcs || !toWcs) {
        from->skyMessage(noSkyText(fromWcs ? to : from));
        return;
    }
    int missed = 0;
    const auto converted = convertRegions(regions, *fromWcs, *toWcs, &missed);
    if (converted.empty()) {
        from->skyMessage(u"None of the regions is on the sky %1 covers."_s.arg(to->skyLinkLabel()));
        return;
    }
    const int added = to->skyAddRegions(converted, u"from %1"_s.arg(from->skyLinkLabel()));
    QString text = u"%1 region(s) sent to %2"_s.arg(added).arg(to->skyLinkLabel());
    if (missed > 0)
        text += u" (%1 not placed: off its projection)"_s.arg(missed);
    from->skyMessage(text + u"."_s);
    to->skyMessage(u"%1 region(s) from %2."_s.arg(added).arg(from->skyLinkLabel()));
    bringForward(to);
}

void SkyLinkHub::extractPv(SkyLinkEndpoint *from)
{
    const auto path = from->skyPvPath();
    if (path.size() < 2) {
        from->skyMessage(u"Select a line region first: its vertices are the path of the PV cut."_s);
        return;
    }
    SkyLinkEndpoint *to = chooseTarget(from, u"Extract PV in"_s, u"Cube window to cut the PV in:"_s);
    if (!to)
        return;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const SkyWcs *fromWcs = from->skyWcs();
    const SkyWcs *toWcs = fromWcs ? to->skyWcs() : nullptr;
    QGuiApplication::restoreOverrideCursor();
    if (!fromWcs || !toWcs) {
        from->skyMessage(noSkyText(fromWcs ? to : from));
        return;
    }
    const auto cubePath = convertPath(path, *fromWcs, *toWcs);
    if (cubePath.size() < 2) {
        from->skyMessage(u"The path is not on the sky %1 covers."_s.arg(to->skyLinkLabel()));
        return;
    }
    if (to->skyRunPv(cubePath, u"from %1"_s.arg(from->skyLinkLabel()))) {
        from->skyMessage(u"PV cut along the path in %1."_s.arg(to->skyLinkLabel()));
        bringForward(to);
    }
}
