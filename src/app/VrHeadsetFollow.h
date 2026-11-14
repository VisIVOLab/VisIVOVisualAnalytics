#pragma once

// VrHeadsetFollow.h
// Follow the headset's look (issue #268): which of the headset's `look` and
// `clip` events a cube window applies, and when.
//
// On by default: a colour map, threshold or clip box (a slab is a clip box
// along the spectral axis) chosen in VR shows on the desktop without looking
// for a setting. The user can turn it off; that choice is kept in the app's
// settings for every cube window opened afterwards.
//
// An event is applied once (its (epoch, seq) is remembered), and only while
// the headset that sent it still shows this cube: a page that has switched to
// another cube since (a newer `cube` event of its own) re-bases its look
// without telling, so its older look is not what it shows any more and is
// never applied. A headset that is not listed as showing this cube yet (the
// presence poll is a few seconds behind the activity) is waited for, not
// skipped: its look is applied when presence catches up.
//
// No feedback loop: the desktop never posts `look` or `clip` events (the
// `show` command it sends carries no box, and a box the page receives is
// re-based on the page, not reported back), so what is applied here cannot
// come back; and an event already applied is not applied a second time.
//
// Widget-free and header-only: unit-tested in tests/test_vr_headset_hub.cpp.

#include "VrHeadsetHub.h"

#include <QPair>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <optional>

namespace VrHub {

/// The app-settings key of Follow the headset's look.
inline QString followLookSettingKey()
{
    return QStringLiteral("vr/followHeadsetLook");
}

/// Follow the headset's look, as the user last chose it (on when never chosen).
inline bool followLookEnabled(const QSettings &settings)
{
    return settings.value(followLookSettingKey(), true).toBool();
}

inline void setFollowLookEnabled(QSettings &settings, bool on)
{
    settings.setValue(followLookSettingKey(), on);
}

/// The (epoch, seq) of the look and the clip a window has dealt with: applied,
/// or found stale. {-1, 0} for none.
struct FollowMarks
{
    QPair<int, qint64> look{ -1, 0 };
    QPair<int, qint64> clip{ -1, 0 };
};

/// What a window applies now: the headset's latest look and clip of its cube,
/// each only when it is new and still on show.
struct FollowStep
{
    std::optional<Event> look;
    std::optional<Event> clip;
    bool any() const { return look || clip; }
};

/// The look and clip of the cube at @p path (or with @p datasetId) to apply
/// now, from @p events (newest first, as ActivityLog keeps them) and the
/// connected @p headsets. @p marks is updated for what is returned and for
/// what turned out stale; an event waiting for presence leaves it unchanged.
inline FollowStep nextToFollow(const QList<Event> &events, const QList<Headset> &headsets,
                               const QString &path, const QString &datasetId, FollowMarks &marks)
{
    FollowStep step;
    bool lookSeen = false;
    bool clipSeen = false;
    // Pages that switched cube after the event being looked at: newest first,
    // so a `cube` event is met before the older looks it makes stale.
    QSet<QString> switchedSince;
    const auto onShow = [&headsets, &path, &datasetId](const Event &e) {
        for (const Headset &h : headsets)
            if ((e.client.isEmpty() || h.client == e.client) && showsCube(h, datasetId, path))
                return true;
        return false;
    };
    for (const Event &e : events) {
        if (lookSeen && clipSeen)
            break;
        if (e.kind == u"cube") {
            if (!e.client.isEmpty())
                switchedSince.insert(e.client);
            continue;
        }
        const bool isLook = e.kind == u"look";
        if ((!isLook && e.kind != u"clip") || !aboutCube(e, path, datasetId))
            continue;
        bool &seen = isLook ? lookSeen : clipSeen;
        if (seen)
            continue;   // only the latest of each counts
        seen = true;
        QPair<int, qint64> &mark = isLook ? marks.look : marks.clip;
        const QPair<int, qint64> id{ e.epoch, e.seq };
        if (id == mark)
            continue;
        if (!e.client.isEmpty() && switchedSince.contains(e.client)) {
            mark = id;   // stale for good: the page has moved on since
            continue;
        }
        if (!onShow(e))
            continue;   // presence has not caught up yet: asked again then
        mark = id;
        (isLook ? step.look : step.clip) = e;
    }
    return step;
}

/// The status bar's word on what was taken from the headset, e.g. "Colour map
/// and threshold from the headset". @p unmatchedColormap: the headset's colour
/// map when the desktop has none of that name. '' when nothing is to be said.
inline QString followedText(bool colormap, bool threshold, bool clip,
                            const QString &unmatchedColormap = {})
{
    QStringList parts;
    if (colormap)
        parts << QStringLiteral("colour map");
    if (threshold)
        parts << QStringLiteral("threshold");
    if (clip)
        parts << QStringLiteral("clip box");
    QString text;
    if (!parts.isEmpty()) {
        const QString last = parts.takeLast();
        text = parts.isEmpty() ? last
                               : QStringLiteral("%1 and %2").arg(parts.join(QStringLiteral(", ")), last);
        text[0] = text[0].toUpper();
        text = QStringLiteral("%1 from the headset").arg(text);
    }
    if (!unmatchedColormap.isEmpty()) {
        const QString none = QStringLiteral("the headset's colour map %1 has no counterpart here")
                                     .arg(unmatchedColormap);
        text = text.isEmpty() ? none.at(0).toUpper() + none.mid(1)
                              : QStringLiteral("%1 (%2)").arg(text, none);
    }
    return text;
}

} // namespace VrHub
