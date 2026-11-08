#include "TestCopilotStatus.h"

#include "CopilotStatus.h"

#include <QSignalSpy>
#include <QTest>

#include <memory>

// The store is a process-wide singleton: every test uses its own backend URLs
// and its own reporters, which are gone when the test ends.

void TestCopilotStatus::report_isReadBackPerBackend()
{
    CopilotStatus *s = CopilotStatus::instance();
    QObject panelA, panelB;
    s->report(&panelA, QStringLiteral("http://a.test:1"), QStringLiteral("No model is set."), s->nextTicket());
    s->report(&panelB, QStringLiteral("http://b.test:1"), QString(), s->nextTicket());
    QCOMPARE(s->problem(QStringLiteral("http://a.test:1")), QStringLiteral("No model is set."));
    // Which configuration it is about travels with it, and goes with it.
    s->report(&panelA, QStringLiteral("http://a.test:1"), QStringLiteral("No model is set."),
              s->nextTicket(), QStringLiteral("server's configuration (self-hosted, no model set)"));
    QCOMPARE(s->active(QStringLiteral("http://a.test:1")),
             QStringLiteral("server's configuration (self-hosted, no model set)"));
    s->report(&panelA, QStringLiteral("http://a.test:1"), QString(), s->nextTicket(),
              QStringLiteral("self-hosted, model qwen"));
    QVERIFY(s->active(QStringLiteral("http://a.test:1")).isEmpty());
    s->report(&panelA, QStringLiteral("http://a.test:1"), QStringLiteral("No model is set."),
              s->nextTicket());
    QVERIFY(s->problem(QStringLiteral("http://b.test:1")).isEmpty());
    QVERIFY(s->problem(QStringLiteral("http://unknown.test:1")).isEmpty());
}

void TestCopilotStatus::report_emptyClearsTheProblem()
{
    // The user set a model: the backend's next reply carries "" and the
    // problem goes, for every panel on that backend.
    CopilotStatus *s = CopilotStatus::instance();
    const QString url = QStringLiteral("http://clear.test:1");
    QObject panel, otherViewer;
    s->report(&panel, url, QStringLiteral("The endpoint serves 2: a, b."), s->nextTicket());
    s->report(&otherViewer, url, QStringLiteral("The endpoint serves 2: a, b."), s->nextTicket());
    s->report(&panel, url, QString(), s->nextTicket());
    QVERIFY(s->problem(url).isEmpty());
    QVERIFY(!s->problems().contains(url));
}

void TestCopilotStatus::problems_listsOnlyBackendsALivePanelUses()
{
    CopilotStatus *s = CopilotStatus::instance();
    const QString url = QStringLiteral("http://closed.test:1");
    auto panel = std::make_unique<QObject>();
    s->report(panel.get(), url, QStringLiteral("Refused base URL."), s->nextTicket());
    QCOMPARE(s->problems().value(url), QStringLiteral("Refused base URL."));

    // The viewer closes: Settings no longer speaks of a backend nobody uses.
    QSignalSpy spy(s, &CopilotStatus::changed);
    panel.reset();
    QVERIFY(!s->problems().contains(url));
    QCOMPARE(spy.count(), 1);

    // A panel that moves to another backend stops counting for the first.
    QObject mover;
    const QString other = QStringLiteral("http://other.test:1");
    s->report(&mover, url, QStringLiteral("Refused base URL."), s->nextTicket());
    s->report(&mover, other, QString(), s->nextTicket());
    QVERIFY(!s->problems().contains(url));

    // Disconnected panels count for nothing either.
    s->report(&mover, url, QStringLiteral("Refused base URL."), s->nextTicket());
    s->forget(&mover);
    QVERIFY(!s->problems().contains(url));
}

namespace {
// A panel that knows when its own destructor has run.
struct DyingPanel : QObject
{
    bool &gone;
    explicit DyingPanel(bool &g) : gone(g) {}
    ~DyingPanel() override { gone = true; }
};
}  // namespace

void TestCopilotStatus::destroyedReporter_doesNotHearItsOwnDeparture()
{
    // A panel listens to changed() too; the one its destruction emits must
    // not reach it, as its members are already gone.
    CopilotStatus *s = CopilotStatus::instance();
    bool gone = false;
    bool heardAfterDeath = false;
    auto *panel = new DyingPanel(gone);
    connect(s, &CopilotStatus::changed, panel, [&]() { heardAfterDeath |= gone; });
    s->report(panel, QStringLiteral("http://dying.test:1"), QStringLiteral("Why."), s->nextTicket());
    QSignalSpy spy(s, &CopilotStatus::changed);
    delete panel;
    QCOMPARE(spy.count(), 1);   // Settings still hears it
    QVERIFY(!heardAfterDeath);
}

void TestCopilotStatus::changed_onlyWhenSomethingChanged()
{
    CopilotStatus *s = CopilotStatus::instance();
    const QString url = QStringLiteral("http://quiet.test:1");
    QObject panel;
    QSignalSpy spy(s, &CopilotStatus::changed);
    s->report(&panel, url, QStringLiteral("Why."), s->nextTicket());
    QCOMPARE(spy.count(), 1);
    s->report(&panel, url, QStringLiteral("Why."), s->nextTicket());   // the same reply again
    QCOMPARE(spy.count(), 1);
    s->report(&panel, url, QString(), s->nextTicket());
    QCOMPARE(spy.count(), 2);
    s->report(nullptr, url, QStringLiteral("ignored"), s->nextTicket());
    s->report(&panel, QString(), QStringLiteral("ignored"), s->nextTicket());
    QCOMPARE(spy.count(), 2);
    QVERIFY(s->problem(url).isEmpty());
}

void TestCopilotStatus::lateReplyFromAnotherPanel_doesNotUndoANewerOne()
{
    // Two viewers on one backend ask it; the first one asked answers last.
    CopilotStatus *s = CopilotStatus::instance();
    const QString url = QStringLiteral("http://order.test:1");
    QObject panelA, panelB;
    const quint64 asked1 = s->nextTicket();   // panel A asks: no problem yet
    const quint64 asked2 = s->nextTicket();   // panel B asks after the first question
    s->report(&panelB, url, QStringLiteral("The endpoint serves 2: a, b."), asked2);
    s->report(&panelA, url, QString(), asked1);
    QCOMPARE(s->problem(url), QStringLiteral("The endpoint serves 2: a, b."));
    // The late reply still counts panel A as a user of that backend.
    s->forget(&panelB);
    QCOMPARE(s->problems().value(url), QStringLiteral("The endpoint serves 2: a, b."));

    // And the other way round: a newer clear wins over an older problem.
    const quint64 asked3 = s->nextTicket();
    const quint64 asked4 = s->nextTicket();   // the user set a model
    s->report(&panelA, url, QString(), asked4);
    s->report(&panelA, url, QStringLiteral("The endpoint serves 2: a, b."), asked3);
    QVERIFY(s->problem(url).isEmpty());
}
