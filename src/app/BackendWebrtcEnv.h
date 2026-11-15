#pragma once

// BackendWebrtcEnv.h
// Headset pairing by code (#212) and the headset's stereo video (#189) need
// the WebRTC packages of backend/requirements-webrtc.txt (aiortc, websockets).
// They are kept out of requirements.txt, which the cluster images install, so
// a user-data venv made from requirements.txt alone (setup_backend under
// 1.0.x, or the first-run bootstrap before 1.1.0) does not have them. The
// launcher prefers that venv over the bundled runtime, which does have them,
// so after an upgrade pairing would answer "not-configured".
//
// The launcher therefore adds them: the bootstrap installs both files, and a
// user-data venv found without aiortc is updated from requirements-webrtc.txt
// before the backend starts. Neither is fatal: without the packages the
// backend runs and only pairing and stereo video are missing, as the
// requirements file says. A failed update (no network) is not tried at every
// start: again with a new version of this client, or a week later.
//
// The update only adds: every package the venv already has is pinned at its
// version (a constraints file from `pip freeze`), so numpy, websockets or
// uvicorn are never moved past what numba and the backend were installed
// with. If aiortc cannot be had without moving one, the install fails, and
// that is the non-fatal case above. pip never asks (no input, a network
// timeout) and the whole update has a finite wait.
//
// Once an environment is known to have the packages, that is remembered (the
// venv and the client version), so the import is not paid at every start.
//
// Widget-free and header-only: unit-tested in tests/test_backend_webrtc_env.cpp.

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace visivo::webrtcenv {

/// What `python -c` runs to tell whether an environment has the packages.
inline QString importCheck()
{
    return QStringLiteral("import aiortc");
}

/// The requirements file, next to requirements.txt in the backend directory.
inline QString requirementsPath(const QString &backendDir)
{
    return backendDir + QStringLiteral("/requirements-webrtc.txt");
}

/// How long a failed update is left alone before it is tried again (ms).
constexpr qint64 kRetryAfterMs = 7LL * 24 * 60 * 60 * 1000;
/// The import check: a cold first import of aiortc (and PyAV) on a slow disk
/// takes many seconds, and a timeout must not read as "missing" (ms).
constexpr int kImportCheckTimeoutMs = 120000;
/// `pip freeze` for the constraints (ms).
constexpr int kFreezeTimeoutMs = 60000;
/// The whole WebRTC update; past it the backend starts without (ms).
constexpr int kUpdateTimeoutMs = 10 * 60 * 1000;
/// pip's own network timeout per connection (s).
constexpr int kPipNetworkTimeoutS = 15;

/// The last update attempt, as remembered in the settings.
struct Attempt
{
    QString version;    ///< the client version that tried ('' never tried)
    QString venv;       ///< the venv it tried on
    qint64 atMs{ 0 };   ///< when (epoch ms)
};

/// The environment found with the packages, as remembered in the settings.
struct Known
{
    QString version;   ///< the client version that saw aiortc import ('' never)
    QString venv;
};

/// Whether the import check is needed at all: not when this client already
/// saw aiortc import in this venv.
inline bool needsCheck(const Known &known, const QString &version, const QString &venv)
{
    return known.version.isEmpty() || known.version != version || known.venv != venv;
}

/// Whether to update the interpreter the backend is about to run with.
/// @p userDataVenv: it is the user-data venv (an interpreter the user set
/// up himself, $VIRTUAL_ENV or VISIVO_BACKEND_PYTHON, is left as it is);
/// @p requirementsFound: requirements-webrtc.txt is there to install from;
/// @p hasWebrtc: it already imports aiortc.
inline bool shouldUpdate(bool userDataVenv, bool requirementsFound, bool hasWebrtc,
                         const Attempt &last, const QString &version, const QString &venv,
                         qint64 nowMs)
{
    if (!userDataVenv || !requirementsFound || hasWebrtc)
        return false;
    if (last.version.isEmpty() || last.version != version || last.venv != venv)
        return true;
    return nowMs - last.atMs >= kRetryAfterMs || nowMs < last.atMs;
}

/// The constraints that keep every installed package where it is: the
/// `name==version` lines of `pip freeze`. Editable installs (`-e …`), direct
/// references (`name @ file://…`) and comments cannot be constraints and are
/// left out; pip leaves those packages alone anyway when it need not touch them.
inline QString constraintsFromFreeze(const QString &freeze)
{
    // pip refuses extras in a constraint ("name[extra]==1"): only the name stays.
    static const QRegularExpression pinned(
            QStringLiteral("^([A-Za-z0-9][A-Za-z0-9._-]*)(\\[[^\\]]*\\])?(==[^\\s;]+)$"));
    QStringList out;
    for (const QString &raw : freeze.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = pinned.match(raw.trimmed());
        if (m.hasMatch())
            out << m.captured(1) + m.captured(3);
    }
    return out.isEmpty() ? QString() : out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

/// The arguments after the interpreter for a pip install of @p requirements.
/// @p upgrade: the bootstrap's full install (upgrades what it lists); the
/// WebRTC update passes false and @p constraints (a file from
/// constraintsFromFreeze), so it only adds. pip never prompts (a proxy's
/// password, a confirmation): it fails instead, with its network timeout.
inline QStringList pipInstallArgs(const QString &requirements, bool upgrade,
                                  const QString &constraints = QString())
{
    QStringList args{ QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install") };
    if (upgrade)
        args << QStringLiteral("--upgrade");
    args << QStringLiteral("--no-input") << QStringLiteral("--disable-pip-version-check")
         << QStringLiteral("--timeout") << QString::number(kPipNetworkTimeoutS);
    if (!constraints.isEmpty())
        args << QStringLiteral("-c") << constraints;
    args << QStringLiteral("-r") << requirements;
    return args;
}

/// The arguments for `pip freeze` (the constraints' source).
inline QStringList pipFreezeArgs()
{
    return { QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("freeze"),
             QStringLiteral("--disable-pip-version-check") };
}

} // namespace visivo::webrtcenv
