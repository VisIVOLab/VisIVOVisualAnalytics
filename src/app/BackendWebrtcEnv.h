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
// Widget-free and header-only: unit-tested in tests/test_backend_webrtc_env.cpp.

#include <QString>

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

/// The last update attempt, as remembered in the settings.
struct Attempt
{
    QString version;    ///< the client version that tried ('' never tried)
    QString venv;       ///< the venv it tried on
    qint64 atMs{ 0 };   ///< when (epoch ms)
};

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

} // namespace visivo::webrtcenv
