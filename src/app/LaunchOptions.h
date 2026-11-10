#ifndef LaunchOptions_h
#define LaunchOptions_h

#include "Settings.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

/**
 * The backend a launch asks for.
 *
 * With nothing given the client runs on its local backend, as it always has. A
 * remote one is chosen explicitly, for that run only:
 *
 *     --backend URL [--backend-token-file FILE] [--backend-name NAME]
 *
 * or VISIVO_BACKEND_URL with VISIVO_BACKEND_TOKEN_FILE or VISIVO_BACKEND_TOKEN,
 * and VISIVO_BACKEND_NAME; the command line wins over the environment. The token
 * is read from a file so that it does not appear in the process list.
 */
namespace LaunchOptions {

/// Options that take a value: their value (a URL, a file, a name) is not a file to open.
const QStringList &backendValueOptions();

/**
 * The remote backend @p args and @p env ask for: @p out is left empty for the
 * local backend.
 *
 * @return false with @p error set when the options cannot be used (an option
 *         without its value, a malformed URL, an unreadable or empty token
 *         file): a mistake to report, never a silent fall-back to local.
 */
bool remoteBackend(const QStringList &args, const QProcessEnvironment &env,
                   std::optional<Settings::SessionBackend> *out, QString *error);

/**
 * The cluster profile `--connect-cluster NAME` asks this run to connect to
 * (empty when not given): the window submits the backend job, opens the
 * tunnel and runs on that backend (ClusterConnection).
 *
 * @return false with @p error set when the option has no value or is combined
 *         with --backend / VISIVO_BACKEND_URL.
 */
bool clusterToConnect(const QStringList &args, const QProcessEnvironment &env, QString *name,
                      QString *error);

/**
 * The datasets @p args ask to open: every argument that is neither an option
 * nor an option's value. On a remote backend (@p remote) a path is the
 * backend's, taken as it is: it may not exist here, and the backend says
 * whether it does. On the local backend only what exists here, made absolute;
 * the rest goes to @p missing (absolute too), to be reported rather than dropped.
 */
QStringList datasetPaths(const QStringList &args, bool remote, QStringList *missing = nullptr);

/**
 * Which running instance a launch may hand its files to: one seat per backend.
 * Empty for the local backend; "backend:URL" for --backend (or
 * VISIVO_BACKEND_URL); "cluster:NAME" for --connect-cluster. A path means
 * something only to its own backend, so a launch on one backend never merges
 * into an instance on another: it finds that seat free and starts its own.
 */
QString instanceSeat(const std::optional<Settings::SessionBackend> &remote, const QString &cluster);

/**
 * The lock and socket name of @p seat for the account whose home is
 * @p homePath. A SHA-256 digest, not qHash (whose algorithm changes across Qt
 * versions) nor $USER (unset in a Finder-started app), truncated to stay
 * inside the ~104-byte Unix-socket path limit. The local seat's key is the
 * one earlier releases used.
 */
QString instanceKey(const QString &homePath, const QString &seat);

/// The recent dataset `--reopen-recent ID` asks the window to reopen once it
/// is up (a window opened on a cluster from another window's history); empty
/// when not given.
QString recentToReopen(const QStringList &args);

}  // namespace LaunchOptions

#endif
