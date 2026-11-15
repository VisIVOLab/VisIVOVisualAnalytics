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
