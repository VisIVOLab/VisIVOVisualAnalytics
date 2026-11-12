#include "TestBackendProcessTree.h"

#include "app/BackendProcessTree.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

using namespace Qt::StringLiterals;
using visivo::BackendProcessTree;
using visivo::RetiredProcessTrees;

namespace {

#ifdef Q_OS_UNIX
bool pidExists(qint64 pid)
{
    return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}

/// Starts @p script under /bin/sh through the launcher's helper, and reads the
/// first @p pids lines it prints, as numbers.
QList<qint64> startTree(QProcess &sh, BackendProcessTree &tree, const QString &script, int pids)
{
    tree.prepare(sh);
    sh.start(u"/bin/sh"_s, { u"-c"_s, script });
    if (!sh.waitForStarted(3000) || !tree.adopt(sh))
        return {};
    QList<qint64> out;
    QElapsedTimer t;
    t.start();
    while (out.size() < pids && t.elapsed() < 5000) {
        if (!sh.canReadLine() && !sh.waitForReadyRead(200))
            continue;
        while (sh.canReadLine())
            out << QString::fromLatin1(sh.readLine()).trimmed().toLongLong();
    }
    return out;
}

/// Whatever a failed test leaves running ends here, not on the user's machine.
/// Only a pid still in the test's own group: once one of ours has gone, its
/// number may belong to anything.
struct KillAtExit
{
    qint64 group;
    QList<qint64> pids;
    ~KillAtExit()
    {
        for (qint64 pid : pids)
            if (pid > 1 && group > 1 && ::getpgid(static_cast<pid_t>(pid)) == group)
                ::kill(static_cast<pid_t>(pid), SIGKILL);
    }
};
#endif

}   // namespace

void TestBackendProcessTree::backendLeadsItsOwnGroup()
{
#ifndef Q_OS_UNIX
    QSKIP("process groups are Unix; Windows uses a job object, untested here");
#else
    QProcess sh;
    BackendProcessTree tree;
    // The shell's pid, its parent's, and its worker's.
    const QList<qint64> pids =
            startTree(sh, tree, u"echo $$; echo $PPID; sleep 60 & echo $!; wait"_s, 3);
    QCOMPARE(pids.size(), 3);
    KillAtExit guard{ sh.processId(), { pids.at(2) } };
    // The launcher's child leads a group of its own, the worker is in it, and
    // the client's group is not: a signal to it cannot reach the client.
    QCOMPARE(tree.groupId(), sh.processId());
    QCOMPARE(qint64(::getpgid(static_cast<pid_t>(pids.at(0)))), sh.processId());
    QCOMPARE(qint64(::getpgid(static_cast<pid_t>(pids.at(2)))), sh.processId());
    QVERIFY(::getpgid(static_cast<pid_t>(pids.at(0))) != ::getpgrp());
    // Its parent is the client itself, not a wrapper: what the backend's
    // parent watch relies on after a client crash.
    QCOMPARE(pids.at(1), QCoreApplication::applicationPid());
    QVERIFY(tree.alive());
    tree.stop(&sh, 3000);
    QVERIFY(!tree.alive());
#endif
}

void TestBackendProcessTree::stopEndsTheWholeTreeAfterAskingFirst()
{
#ifndef Q_OS_UNIX
    QSKIP("process groups are Unix; Windows uses a job object, untested here");
#else
    QProcess sh;
    BackendProcessTree tree;
    const QList<qint64> pids = startTree(sh, tree, u"sleep 60 & echo $!; wait"_s, 1);
    QCOMPARE(pids.size(), 1);
    KillAtExit guard{ sh.processId(), pids };
    QVERIFY(pidExists(pids.at(0)));

    QElapsedTimer t;
    t.start();
    tree.stop(&sh, 3000);
    // SIGTERM was enough for both: nothing waited out the grace period.
    QVERIFY2(t.elapsed() < 2000, qPrintable(u"took %1 ms"_s.arg(t.elapsed())));
    QCOMPARE(sh.state(), QProcess::NotRunning);
    QTRY_VERIFY_WITH_TIMEOUT(!pidExists(pids.at(0)), 2000);
    QVERIFY(!tree.alive());
    QCOMPARE(tree.groupId(), 0);
#endif
}

void TestBackendProcessTree::stopKillsAWorkerThatIgnoresTerm()
{
#ifndef Q_OS_UNIX
    QSKIP("process groups are Unix; Windows uses a job object, untested here");
#else
    // The worker inherits an ignored SIGTERM (a pool worker inside a long
    // native call behaves the same); the shell itself goes on SIGTERM. The
    // server's exit is not the end: the worker is still in the group.
    QProcess sh;
    BackendProcessTree tree;
    const QList<qint64> pids =
            startTree(sh, tree, u"trap '' TERM; sleep 60 & echo $!; trap - TERM; wait"_s, 1);
    QCOMPARE(pids.size(), 1);
    KillAtExit guard{ sh.processId(), pids };

    QElapsedTimer t;
    t.start();
    tree.stop(&sh, 500);
    QCOMPARE(sh.state(), QProcess::NotRunning);
    QTRY_VERIFY_WITH_TIMEOUT(!pidExists(pids.at(0)), 2000);
    // Bounded: the grace period, then the kill, then the reap.
    QVERIFY2(t.elapsed() < 3000, qPrintable(u"took %1 ms"_s.arg(t.elapsed())));
    QVERIFY(!tree.alive());
#endif
}

void TestBackendProcessTree::stopEndsWorkersTheServerLeftBehind()
{
#ifndef Q_OS_UNIX
    QSKIP("process groups are Unix; Windows uses a job object, untested here");
#else
    // The server exits by itself (a crash) and leaves its worker running with
    // parent PID 1 — what #176 found nine days later. The quit still ends it.
    // The killed orphan must then be reaped by PID 1. In a container whose
    // PID 1 is this test, or another process that never reaps, it stays a
    // zombie, which kill(pid, 0) still finds: not a failure of the tree.
    if (::getpid() == 1)
        QSKIP("this process is PID 1: nothing reaps the orphan");
    QProcess sh;
    BackendProcessTree tree;
    const QList<qint64> pids = startTree(sh, tree, u"sleep 60 & echo $!"_s, 1);
    QCOMPARE(pids.size(), 1);
    KillAtExit guard{ sh.processId(), pids };
    QVERIFY(sh.waitForFinished(3000));
    QVERIFY(pidExists(pids.at(0)));
    QVERIFY(tree.alive());

    tree.stop(&sh, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!pidExists(pids.at(0)), 2000);
    QVERIFY(!tree.alive());
#endif
}

void TestBackendProcessTree::stopWithNoTreeIsHarmless()
{
    BackendProcessTree tree;
    QVERIFY(!tree.alive());
    tree.stop(nullptr, 100);
    QProcess never;
    tree.stop(&never, 100);
    QVERIFY(!tree.alive());
}

// ── RetiredProcessTrees ──────────────────────────────────────────────────────

void TestBackendProcessTree::retiredTreeEndsWithoutTouchingTheCurrentOne()
{
#ifndef Q_OS_UNIX
    QSKIP("process groups are Unix; Windows uses a job object, untested here");
#else
    // The Retry case: a first backend never answered and is replaced. Its tree
    // is retired (its worker ignores SIGTERM, so it must be killed at the
    // deadline) while a second backend starts in a tree of its own. Ending
    // the first must not reach the second, however the first one ends.
    QProcess oldSh;
    auto oldTree = std::make_unique<BackendProcessTree>();
    const QList<qint64> oldPids = startTree(
            oldSh, *oldTree, u"trap '' TERM; sleep 60 & echo $!; trap - TERM; wait"_s, 1);
    QCOMPARE(oldPids.size(), 1);
    KillAtExit oldGuard{ oldSh.processId(), oldPids };

    RetiredProcessTrees retired;
    retired.retire(std::move(oldTree), 500);
    QCOMPARE(retired.size(), 1);

    QProcess newSh;
    BackendProcessTree newTree;
    const QList<qint64> newPids = startTree(newSh, newTree, u"sleep 60 & echo $!; wait"_s, 1);
    QCOMPARE(newPids.size(), 1);
    KillAtExit newGuard{ newSh.processId(), newPids };
    QVERIFY(newTree.groupId() != oldGuard.group);

    // What BackendLauncher's timer does, every 50 ms.
    QElapsedTimer t;
    t.start();
    while (retired.tick() && t.elapsed() < 3000)
        QTest::qWait(50);
    QVERIFY(retired.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!pidExists(oldPids.at(0)), 2000);
    QVERIFY(t.elapsed() >= 400);   // the worker had its grace

    // The current backend and its worker are untouched.
    QCOMPARE(newSh.state(), QProcess::Running);
    QVERIFY(pidExists(newPids.at(0)));
    QVERIFY(newTree.alive());
    newTree.stop(&newSh, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!pidExists(newPids.at(0)), 2000);
#endif
}

void TestBackendProcessTree::retiringAnEmptyTreeKeepsNothing()
{
    RetiredProcessTrees retired;
    retired.retire(nullptr, 1000);
    retired.retire(std::make_unique<BackendProcessTree>(), 1000);
    QVERIFY(retired.isEmpty());
    QVERIFY(!retired.tick());
#ifdef Q_OS_UNIX
    // A tree that has ended by itself is dropped at the next look, so its
    // group id is never signalled once it may be someone else's.
    QProcess sh;
    auto tree = std::make_unique<BackendProcessTree>();
    const QList<qint64> pids = startTree(sh, *tree, u"echo $$; sleep 0.2"_s, 1);
    QCOMPARE(pids.size(), 1);
    retired.retire(std::move(tree), 5000);
    QCOMPARE(retired.size(), 1);
    QVERIFY(sh.waitForFinished(3000));
    // Not at once: the orphaned sleep is reaped by launchd/init, not by us.
    QTRY_VERIFY_WITH_TIMEOUT(!retired.tick(), 2000);
    QVERIFY(retired.isEmpty());
#endif
}

void TestBackendProcessTree::quitEndsRetiredTrees()
{
#ifndef Q_OS_UNIX
    QSKIP("process groups are Unix; Windows uses a job object, untested here");
#else
    QProcess sh;
    auto tree = std::make_unique<BackendProcessTree>();
    const QList<qint64> pids =
            startTree(sh, *tree, u"trap '' TERM; sleep 60 & echo $!; trap - TERM; wait"_s, 1);
    QCOMPARE(pids.size(), 1);
    KillAtExit guard{ sh.processId(), pids };
    RetiredProcessTrees retired;
    // The shell goes on the SIGTERM retire() sends; with no event loop running
    // nothing reaps it but quit() itself, and a zombie leader keeps its group
    // alive: without the leader, quit() would wait the grace, kill, then wait
    // a further second for a zombie that never goes.
    retired.retire(std::move(tree), 300, &sh);
    QElapsedTimer t;
    t.start();
    retired.quit();
    QVERIFY(retired.isEmpty());
    QCOMPARE(sh.state(), QProcess::NotRunning);   // reaped by quit()
    QTRY_VERIFY_WITH_TIMEOUT(!pidExists(pids.at(0)), 2000);
    QVERIFY2(t.elapsed() < 1000, qPrintable(u"took %1 ms"_s.arg(t.elapsed())));
#endif
}
