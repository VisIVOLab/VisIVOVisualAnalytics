#include "SecureStore.h"

#include <QByteArray>

#if defined(Q_OS_MACOS)
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX   // windows.h's min/max macros break std::min / std::max elsewhere
#endif
#include <string>
#include <windows.h>
#include <wincred.h>
#elif defined(VISIVO_HAVE_LIBSECRET)
// GLib's D-Bus headers have a struct member named `signals`, which Qt defines
// as a macro: hide it while they are read.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#endif

namespace visivo::secure {

#if defined(Q_OS_MACOS)

namespace {

/// A CFDictionary describing the generic-password item (service, account).
CFMutableDictionaryRef baseQuery(const QString &service, const QString &account)
{
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef svc = service.toCFString();
    CFStringRef acc = account.toCFString();
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, svc);
    CFDictionarySetValue(q, kSecAttrAccount, acc);
    CFRelease(svc);
    CFRelease(acc);
    return q;
}

}   // namespace

bool available()
{
    return true;
}

bool write(const QString &service, const QString &account, const QString &secret)
{
    const QByteArray bytes = secret.toUtf8();
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8 *>(bytes.constData()), bytes.size());
    // Update the item in place if it exists, so a failed write never leaves
    // the user with no stored sign-in at all.
    CFMutableDictionaryRef query = baseQuery(service, account);
    CFMutableDictionaryRef changes = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(changes, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, changes);
    CFRelease(changes);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        // Readable only while the Mac is unlocked, and never synced or migrated.
        CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
        status = SecItemAdd(query, nullptr);
    }
    CFRelease(query);
    CFRelease(data);
    return status == errSecSuccess;
}

QString read(const QString &service, const QString &account)
{
    CFMutableDictionaryRef q = baseQuery(service, account);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(q, &result);
    CFRelease(q);
    if (status != errSecSuccess || !result)
        return QString();
    CFDataRef data = static_cast<CFDataRef>(result);
    const QString secret = QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                                             CFDataGetLength(data));
    CFRelease(result);
    return secret;
}

bool remove(const QString &service, const QString &account)
{
    CFMutableDictionaryRef q = baseQuery(service, account);
    const OSStatus status = SecItemDelete(q);
    CFRelease(q);
    return status == errSecSuccess || status == errSecItemNotFound;
}

#elif defined(Q_OS_WIN)

namespace {
std::wstring target(const QString &service, const QString &account)
{
    return (service + u'/' + account).toStdWString();
}
}   // namespace

bool available()
{
    return true;
}

bool write(const QString &service, const QString &account, const QString &secret)
{
    const std::wstring name = target(service, account);
    const QByteArray bytes = secret.toUtf8();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(name.c_str());
    cred.CredentialBlobSize = DWORD(bytes.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(bytes.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) == TRUE;
}

QString read(const QString &service, const QString &account)
{
    const std::wstring name = target(service, account);
    PCREDENTIALW cred = nullptr;
    if (CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &cred) != TRUE || !cred)
        return QString();
    const QString secret = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                             int(cred->CredentialBlobSize));
    CredFree(cred);
    return secret;
}

bool remove(const QString &service, const QString &account)
{
    const std::wstring name = target(service, account);
    if (CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0) == TRUE)
        return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

#elif defined(VISIVO_HAVE_LIBSECRET)

namespace {
const SecretSchema *schema()
{
    static const SecretSchema s = {
        "it.inaf.oact.VisIVO.Token",
        SECRET_SCHEMA_NONE,
        { { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
          { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
          { nullptr, SecretSchemaAttributeType(0) } },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return &s;
}
}   // namespace

bool available()
{
    return true;
}

bool write(const QString &service, const QString &account, const QString &secret)
{
    const QByteArray svc = service.toUtf8(), acc = account.toUtf8(), sec = secret.toUtf8();
    const QByteArray label = "VisIVO Visual Analytics: " + svc;
    GError *error = nullptr;
    const gboolean ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT,
                                                   label.constData(), sec.constData(), nullptr,
                                                   &error, "service", svc.constData(), "account",
                                                   acc.constData(), nullptr);
    if (error)
        g_error_free(error);
    return ok;
}

QString read(const QString &service, const QString &account)
{
    const QByteArray svc = service.toUtf8(), acc = account.toUtf8();
    GError *error = nullptr;
    gchar *pw = secret_password_lookup_sync(schema(), nullptr, &error, "service", svc.constData(),
                                            "account", acc.constData(), nullptr);
    if (error)
        g_error_free(error);
    if (!pw)
        return QString();
    const QString secret = QString::fromUtf8(pw);
    secret_password_free(pw);
    return secret;
}

bool remove(const QString &service, const QString &account)
{
    const QByteArray svc = service.toUtf8(), acc = account.toUtf8();
    GError *error = nullptr;
    secret_password_clear_sync(schema(), nullptr, &error, "service", svc.constData(), "account",
                               acc.constData(), nullptr);
    const bool ok = !error;
    if (error)
        g_error_free(error);
    return ok;
}

#else   // no secure store in this build

bool available()
{
    return false;
}

bool write(const QString &, const QString &, const QString &)
{
    return false;
}

QString read(const QString &, const QString &)
{
    return QString();
}

bool remove(const QString &, const QString &)
{
    return true;
}

#endif

}   // namespace visivo::secure
