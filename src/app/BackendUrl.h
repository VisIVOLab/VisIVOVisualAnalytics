#ifndef BackendUrl_h
#define BackendUrl_h

#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * Is a configured backend URL served by the machine the client runs on?
 *
 * The client starts a backend of its own when the configured URL answers
 * nothing. That is right for the default (a loopback URL and no server yet) and
 * wrong for a remote one: uvicorn is told to bind the *remote* host, the bind
 * fails, and the user is shown a generic start-up failure instead of being told
 * that the server they configured did not answer.
 *
 * Two deliberate limits on how much this claims to know.
 *
 * It is NOT a loopback test. Pointing the client at an address this machine
 * holds is a working setup — it is how a colleague on the same network reaches
 * your backend — and it autostarts correctly, because binding an address this
 * machine holds succeeds. A loopback-only test would take that away.
 *
 * And it answers Unknown rather than guessing. A machine called `visivo01` is
 * routinely addressed as `visivo01.inaf.it`, or by an alias in /etc/hosts, and
 * uvicorn binds those happily; a name that merely *looks* like ours may belong
 * to a different host entirely. Deciding either way from the spelling would be
 * a guess, and a wrong guess in the refusing direction takes away a setup that
 * works today. Only an address literal gives certainty, so only an address
 * literal produces NotThisMachine.
 */
namespace BackendUrl {

enum class Locality {
    ThisMachine,      ///< Certainly ours: the bind will succeed.
    NotThisMachine,   ///< Certainly not: an address this machine does not hold.
    Unknown,          ///< A name we cannot judge from its spelling alone.
    Malformed,        ///< Text that is not a URL at all; nothing can use it.
};

/** The host of @p url, lowercased. Empty when the URL carries none. */
QString hostOf(const QString &url);

/** The port of @p url, or @p fallback. Parsed exactly as hostOf() parses. */
int portOf(const QString &url, int fallback);

/** Names this machine answers to without asking anyone. Lowercase. */
QStringList thisMachineNames();

/**
 * The decision, from an explicit picture of the machine. Pure, and touches no
 * network at all: no DNS lookup, so nothing here can hang or time out.
 *
 * @param url            the configured backend URL
 * @param localAddresses addresses this machine holds (any interface)
 * @param localNames     names this machine answers to, lowercase
 */
Locality localityOf(const QString &url,
                    const QList<QHostAddress> &localAddresses,
                    const QStringList &localNames);

/** localityOf() against this machine as it actually is. */
Locality localityOf(const QString &url);

}   // namespace BackendUrl

#endif   // BackendUrl_h
