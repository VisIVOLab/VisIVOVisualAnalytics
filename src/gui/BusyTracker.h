#pragma once

// BusyTracker.h
// What a window's busy indicator says, and when it shows (issue #159).
//
// Several operations can be busy on one window at once, and they end in any
// order: a VBT table loading on a worker while another file is opened, a
// mosaic while a cube opens. Each operation is an entry of its own, addressed
// by the token begin() hands out: its outcome is said in its own words, and
// the indicator stays up while any entry is still running.
//
// One operation also runs through nested steps (Recent ▸ reopen ▸ classify ▸
// open), each of which holds a scope. A step begun synchronously inside
// another (same event-loop level, the outer one still running) JOINS it rather
// than making an entry of its own, so its outcome is the operation's and ends
// it at once (before an error box, not when the outer function returns). A
// scope begun from a nested event loop (a click handled while an open waits on
// the backend) is a deeper level, and so an operation of its own; so is one
// begun `detached` (held past its function by a callback).
//
// A paused tracker shows nothing: a modal box is up and is the thing to read.
// Widget-free, so the rules are unit-tested; BusyStatus.h puts them on a
// window's status bar.

#include <QList>
#include <QString>

#include <optional>

namespace visivo {

enum class BusyOutcome { Done, Failed, Cancelled };

/// The sentences of one busy operation: while it runs, and for each way it ends.
struct BusyTexts
{
    QString busy;        ///< "Opening sample.csv…"
    QString done;        ///< "Opened sample.csv."
    QString failed;      ///< "Could not open sample.csv."
    QString cancelled;   ///< "Opening sample.csv cancelled."
};

/// The texts of opening the file @p name.
inline BusyTexts openingTexts(const QString &name)
{
    return { QStringLiteral("Opening %1…").arg(name), QStringLiteral("Opened %1.").arg(name),
             QStringLiteral("Could not open %1.").arg(name),
             QStringLiteral("Opening %1 cancelled.").arg(name) };
}

/// The texts of a task named by @p what ("Building the mosaic"), its end said
/// as "<what>: done." and so on.
inline BusyTexts taskTexts(const QString &what)
{
    return { QStringLiteral("%1…").arg(what), QStringLiteral("%1: done.").arg(what),
             QStringLiteral("%1: failed.").arg(what),
             QStringLiteral("%1: cancelled.").arg(what) };
}

class BusyTracker
{
public:
    /// A scope starts at event-loop level @p loopLevel. Returns its token:
    /// that of the running operation it joins, or of a new one.
    int begin(const BusyTexts &texts, int loopLevel = 0, bool detached = false)
    {
        if (!detached) {
            for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
                if (it->running() && !it->detached && it->loopLevel == loopLevel) {
                    ++it->scopes;
                    return it->token;
                }
            }
        }
        Entry e;
        e.token = ++m_lastToken;
        e.texts = texts;
        e.loopLevel = loopLevel;
        e.detached = detached;
        m_entries.push_back(e);
        return e.token;
    }

    /// Whether the indicator is up: an operation runs, and no box is up.
    bool showing() const { return m_pauses == 0 && anyRunning(); }

    /// What the indicator says: the latest running operation's busy text.
    QString busyText() const
    {
        for (auto it = m_entries.crbegin(); it != m_entries.crend(); ++it)
            if (it->running())
                return it->texts.busy;
        return {};
    }

    /// The outcome of the operation @p token belongs to, in its own words,
    /// with @p detail after it when given. Only its first outcome counts.
    std::optional<QString> finish(int token, BusyOutcome outcome, const QString &detail = {})
    {
        Entry *e = find(token);
        if (!e || !e->running())
            return std::nullopt;
        e->finished = true;
        const BusyTexts &t = e->texts;
        QString text = outcome == BusyOutcome::Done     ? t.done
                       : outcome == BusyOutcome::Failed ? t.failed
                                                        : t.cancelled;
        const QString more = detail.trimmed();
        if (!more.isEmpty()) {
            if (text.endsWith(u'.'))
                text.chop(1);
            text += QStringLiteral(": ") + more;
        }
        return text;
    }

    struct EndAction
    {
        /// The operation ended with no outcome said: its busy text, to clear
        /// from the status bar if it is still what the bar says (else empty).
        QString clearText;
    };

    /// A scope of @p token ends; its operation goes once all its scopes have.
    EndAction end(int token)
    {
        EndAction action;
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.token != token)
                continue;
            if (--e.scopes > 0)
                return action;
            if (!e.finished)
                action.clearText = e.texts.busy;
            m_entries.removeAt(i);
            return action;
        }
        return action;
    }

    /// A modal box comes up (pause) and goes (resume): nothing shows between.
    void pause() { ++m_pauses; }
    void resume()
    {
        if (m_pauses > 0)
            --m_pauses;
    }
    bool paused() const { return m_pauses > 0; }

    /// Operations still listed (running or finished, scopes not all ended).
    int operations() const { return static_cast<int>(m_entries.size()); }

private:
    struct Entry
    {
        int token{ 0 };
        BusyTexts texts;
        int loopLevel{ 0 };
        bool detached{ false };
        bool finished{ false };
        int scopes{ 1 };
        bool running() const { return !finished; }
    };

    bool anyRunning() const
    {
        for (const Entry &e : m_entries)
            if (e.running())
                return true;
        return false;
    }

    Entry *find(int token)
    {
        for (Entry &e : m_entries)
            if (e.token == token)
                return &e;
        return nullptr;
    }

    QList<Entry> m_entries;
    int m_lastToken{ 0 };
    int m_pauses{ 0 };
};

}  // namespace visivo
