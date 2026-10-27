#ifndef SecureStore_H
#define SecureStore_H

// SecureStore.h
// A secret kept in the platform's own secure store, so a sign-in survives a
// restart without ever touching the plain-text settings file:
//   * macOS   — the login Keychain (Security framework, generic password);
//   * Windows — the Credential Manager (wincred, generic credential);
//   * Linux   — the Secret Service through libsecret, when the build found it.
// Where none is available (Linux without libsecret) every call reports failure
// and the caller simply signs in again on the next start.
//
// The calls are synchronous: each is a single local lookup.

#include <QString>

namespace visivo::secure {

/// Whether this build has a secure store at all.
bool available();

/// Store @p secret under (@p service, @p account), replacing any previous one.
bool write(const QString &service, const QString &account, const QString &secret);

/// The secret stored under (@p service, @p account), or an empty string.
QString read(const QString &service, const QString &account);

/// Forget the secret stored under (@p service, @p account). True if it is gone,
/// including when there was none.
bool remove(const QString &service, const QString &account);

}   // namespace visivo::secure

#endif   // SecureStore_H
