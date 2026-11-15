#include "TestBackendWebrtcEnv.h"

#include "app/BackendWebrtcEnv.h"

#include <QFileInfo>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace W = visivo::webrtcenv;

namespace {
constexpr qint64 kNow = 1'800'000'000'000LL;
const QString kVenv = u"/home/u/.local/share/VisIVO/.venv"_s;
} // namespace

void TestBackendWebrtcEnv::onlyTheUserDataVenvWithoutAiortcIsUpdated()
{
    const W::Attempt never;
    // A venv made from requirements.txt alone, before 1.1.0: updated.
    QVERIFY(W::shouldUpdate(true, true, false, never, u"1.1.0"_s, kVenv, kNow));
    // It already has the packages.
    QVERIFY(!W::shouldUpdate(true, true, true, never, u"1.1.0"_s, kVenv, kNow));
    // An interpreter the user chose ($VIRTUAL_ENV, VISIVO_BACKEND_PYTHON, the
    // bundled runtime): left as it is.
    QVERIFY(!W::shouldUpdate(false, true, false, never, u"1.1.0"_s, kVenv, kNow));
    // Nothing to install from.
    QVERIFY(!W::shouldUpdate(true, false, false, never, u"1.1.0"_s, kVenv, kNow));
}

void TestBackendWebrtcEnv::aFailedUpdateWaitsForANewVersionOrAWeek()
{
    // Offline at the first start after the upgrade: not tried at every start.
    const W::Attempt failed{ u"1.1.0"_s, kVenv, kNow - 60'000 };
    QVERIFY(!W::shouldUpdate(true, true, false, failed, u"1.1.0"_s, kVenv, kNow));
    QVERIFY(!W::shouldUpdate(true, true, false, failed, u"1.1.0"_s, kVenv,
                             failed.atMs + W::kRetryAfterMs - 1));
    // A week on, or a newer client, or another venv: tried again.
    QVERIFY(W::shouldUpdate(true, true, false, failed, u"1.1.0"_s, kVenv,
                            failed.atMs + W::kRetryAfterMs));
    QVERIFY(W::shouldUpdate(true, true, false, failed, u"1.1.1"_s, kVenv, kNow));
    QVERIFY(W::shouldUpdate(true, true, false, failed, u"1.1.0"_s, u"/other/.venv"_s, kNow));
    // A clock set back below the attempt does not hold it off for ever.
    QVERIFY(W::shouldUpdate(true, true, false, failed, u"1.1.0"_s, kVenv, failed.atMs - 1));
}

void TestBackendWebrtcEnv::theRequirementsSitNextToTheBackends()
{
    QCOMPARE(W::requirementsPath(u"/opt/visivo/backend"_s), u"/opt/visivo/backend/requirements-webrtc.txt"_s);
    QCOMPARE(W::importCheck(), u"import aiortc"_s);
    // The file this checks for is the one the repository ships.
    const QString backend = QString::fromUtf8(VISIVO_REPO_ROOT) + u"/backend"_s;
    if (!QFileInfo::exists(backend + u"/requirements.txt"_s))
        QSKIP("the backend is not in this tree");
    const QString file = W::requirementsPath(backend);
    QVERIFY2(QFileInfo::exists(file), qPrintable(file));
}

void TestBackendWebrtcEnv::aVenvSeenWithAiortcIsNotCheckedAgain()
{
    // The import costs seconds: once seen working, not paid at every start.
    QVERIFY(W::needsCheck({}, u"1.1.0"_s, kVenv));
    const W::Known seen{ u"1.1.0"_s, kVenv };
    QVERIFY(!W::needsCheck(seen, u"1.1.0"_s, kVenv));
    // A new client, or another venv: checked again.
    QVERIFY(W::needsCheck(seen, u"1.1.1"_s, kVenv));
    QVERIFY(W::needsCheck(seen, u"1.1.0"_s, u"/other/.venv"_s));
}

void TestBackendWebrtcEnv::theUpdatePinsWhatIsInstalled()
{
    // Everything installed is pinned, so adding aiortc cannot move numpy past
    // numba's bounds (or websockets and uvicorn past the backend's).
    const QString freeze = u"numpy==1.26.4\n"
                           u"numba==0.60.0\r\n"
                           u"uvicorn[standard]==0.30.1\n"
                           u"websockets==11.0.3\n"
                           u"-e git+https://example.org/x.git@abc#egg=x\n"
                           u"mypkg @ file:///home/u/mypkg\n"
                           u"# a comment\n"
                           u"\n"
                           u"zope.interface==6.4.post2\n"_s;
    QCOMPARE(W::constraintsFromFreeze(freeze),
             u"numpy==1.26.4\nnumba==0.60.0\nuvicorn==0.30.1\nwebsockets==11.0.3\n"
             u"zope.interface==6.4.post2\n"_s);
    QVERIFY(W::constraintsFromFreeze(u"\n-e .\n"_s).isEmpty());
}

void TestBackendWebrtcEnv::pipNeverPromptsAndOnlyTheBootstrapUpgrades()
{
    const QStringList add = W::pipInstallArgs(u"/b/requirements-webrtc.txt"_s, false, u"/t/c.txt"_s);
    QCOMPARE(add, QStringList({ u"-m"_s, u"pip"_s, u"install"_s, u"--no-input"_s,
                                u"--disable-pip-version-check"_s, u"--timeout"_s, u"15"_s,
                                u"-c"_s, u"/t/c.txt"_s, u"-r"_s, u"/b/requirements-webrtc.txt"_s }));
    QVERIFY(!add.contains(u"--upgrade"_s));
    const QStringList bootstrap = W::pipInstallArgs(u"/b/requirements.txt"_s, true);
    QVERIFY(bootstrap.contains(u"--upgrade"_s));
    QVERIFY(bootstrap.contains(u"--no-input"_s));
    QVERIFY(!bootstrap.contains(u"-c"_s));
    QCOMPARE(bootstrap.last(), u"/b/requirements.txt"_s);
    QCOMPARE(W::pipFreezeArgs(), QStringList({ u"-m"_s, u"pip"_s, u"freeze"_s,
                                               u"--disable-pip-version-check"_s }));
    // The whole update ends: the backend starts without WebRTC past it.
    QVERIFY(W::kUpdateTimeoutMs > 0 && W::kUpdateTimeoutMs <= 15 * 60 * 1000);
}
