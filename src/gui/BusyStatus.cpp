#include "BusyStatus.h"

#include <QApplication>
#include <QList>
#include <QMainWindow>
#include <QProgressBar>
#include <QStatusBar>
#include <QThread>

using namespace Qt::StringLiterals;

namespace visivo {

namespace {

/// BusyPause scopes alive now: every indicator, even one made meanwhile, is
/// paused that many times.
int g_pauses = 0;

/// A window's indicator and the tracker behind it: one per window, a child
/// of it, created on the first busy scope.
class BusyIndicator : public QObject
{
public:
    explicit BusyIndicator(QMainWindow *window) : QObject(window), m_window(window)
    {
        setObjectName(u"visivoBusyIndicator"_s);
        for (int i = 0; i < g_pauses; ++i)
            m_tracker.pause();
        all().push_back(this);
        // An outcome's line times out (or someone clears the bar) while
        // another operation still runs: say that one again.
        QObject::connect(window->statusBar(), &QStatusBar::messageChanged, this,
                         [this](const QString &message) {
                             if (message.isEmpty() && m_tracker.showing())
                                 say(m_tracker.busyText(), 0);
                         });
    }
    ~BusyIndicator() override { all().removeAll(this); }

    static QList<BusyIndicator *> &all()
    {
        static QList<BusyIndicator *> indicators;
        return indicators;
    }

    static BusyIndicator *of(QMainWindow *window)
    {
        // By name, as a QObject: the class has no meta-object of its own for
        // qobject_cast, and only this file makes objects of that name.
        QObject *found = window->findChild<QObject *>(u"visivoBusyIndicator"_s,
                                                      Qt::FindDirectChildrenOnly);
        return found ? static_cast<BusyIndicator *>(found) : new BusyIndicator(window);
    }

    int begin(const BusyTexts &texts, bool detached)
    {
        const int token =
                m_tracker.begin(texts, QThread::currentThread()->loopLevel(), detached);
        sync();
        if (m_tracker.showing())
            say(m_tracker.busyText(), 0);
        return token;
    }

    void finish(int token, BusyOutcome outcome, const QString &detail)
    {
        const std::optional<QString> text = m_tracker.finish(token, outcome, detail);
        if (!text)
            return;
        sync();
        say(*text, outcome == BusyOutcome::Done ? 8000 : 12000);
    }

    void end(int token)
    {
        const BusyTracker::EndAction action = m_tracker.end(token);
        // Only our own words: a message someone else put up meanwhile stays.
        if (!action.clearText.isEmpty() && m_window
            && m_window->statusBar()->currentMessage() == action.clearText)
            m_window->statusBar()->clearMessage();
        sync();   // another operation running on is said again by messageChanged
    }

    void pause()
    {
        m_tracker.pause();
        sync();
    }
    void resume()
    {
        m_tracker.resume();
        sync();
        if (m_tracker.showing())
            say(m_tracker.busyText(), 0);
    }

private:
    /// The bar and the cursor follow the tracker.
    void sync()
    {
        if (!m_window)
            return;
        const bool on = m_tracker.showing();
        if (on && !m_bar) {
            m_bar = new QProgressBar(m_window->statusBar());
            m_bar->setRange(0, 0);   // no steps: a moving bar
            m_bar->setTextVisible(false);
            m_bar->setMaximumWidth(120);
            m_bar->setMaximumHeight(12);
            m_window->statusBar()->addPermanentWidget(m_bar);
        }
        if (m_bar)
            m_bar->setVisible(on);
        if (on == m_cursorSet)
            return;
        m_cursorSet = on;
        if (on)
            m_window->setCursor(Qt::BusyCursor);
        else
            m_window->unsetCursor();
    }

    void say(const QString &text, int timeoutMs)
    {
        if (m_window && !text.isEmpty())
            m_window->statusBar()->showMessage(text, timeoutMs);
    }

    QPointer<QMainWindow> m_window;
    QPointer<QProgressBar> m_bar;
    BusyTracker m_tracker;
    bool m_cursorSet{ false };
};

}  // namespace

BusyStatus::BusyStatus(QMainWindow *window, const BusyTexts &texts, Scope scope)
    : m_window(window)
{
    if (m_window)
        m_token = BusyIndicator::of(m_window)->begin(texts, scope == Scope::Detached);
}

BusyStatus::~BusyStatus()
{
    if (m_window)
        BusyIndicator::of(m_window)->end(m_token);
}

void BusyStatus::finish(BusyOutcome outcome, const QString &detail)
{
    if (m_window)
        BusyIndicator::of(m_window)->finish(m_token, outcome, detail);
}

BusyPause::BusyPause()
{
    ++g_pauses;
    for (BusyIndicator *indicator : BusyIndicator::all())
        indicator->pause();
}

BusyPause::~BusyPause()
{
    --g_pauses;
    for (BusyIndicator *indicator : BusyIndicator::all())
        indicator->resume();
}

QMainWindow *busyWindowFor(QMainWindow *fallback)
{
    if (auto *active = qobject_cast<QMainWindow *>(QApplication::activeWindow()))
        return active;
    return fallback;
}

}  // namespace visivo
