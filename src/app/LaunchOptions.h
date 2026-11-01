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

}  // namespace LaunchOptions

#endif
