#include "TestBackendUrlLocality.h"

#include <QTest>

#include "BackendUrl.h"

using BackendUrl::Locality;

namespace {

/// A fixed machine: one Wi-Fi address, loopback, a link-local. Fixed
/// deliberately — asking the real host would make the result depend on whether
/// the laptop running the tests has Wi-Fi up.
QList<QHostAddress> machineAddresses()
{
    return { QHostAddress(QStringLiteral("192.168.1.42")),
             QHostAddress(QStringLiteral("127.0.0.1")),
             QHostAddress(QStringLiteral("fe80::1")) };
}

/// What thisMachineNames() derives for a host reporting "fabios-mac.local".
QStringList machineNames()
{
    return { QStringLiteral("fabios-mac.local"), QStringLiteral("fabios-mac") };
}

Locality locality(const QString &url)
{
    return BackendUrl::localityOf(url, machineAddresses(), machineNames());
}

}   // namespace

void TestBackendUrlLocality::loopbackIsThisMachine()
{
    QCOMPARE(locality(QStringLiteral("http://127.0.0.1:8000")), Locality::ThisMachine);
    QCOMPARE(locality(QStringLiteral("http://localhost:8000")), Locality::ThisMachine);
    QCOMPARE(locality(QStringLiteral("http://[::1]:8000")), Locality::ThisMachine);
}

void TestBackendUrlLocality::theWildcardBindIsThisMachine()
{
    // "Serve on every interface" is a thing people configure; it is not a
    // remote host, and refusing to start a backend for it would be wrong.
    QCOMPARE(locality(QStringLiteral("http://0.0.0.0:8000")), Locality::ThisMachine);
}

void TestBackendUrlLocality::anAddressThisMachineHoldsIsThisMachine()
{
    // The regression this test exists for: pointing the client at your own LAN
    // address, so a colleague can reach your backend, autostarts correctly
    // today. A loopback-only check would have silently taken that away.
    QCOMPARE(locality(QStringLiteral("http://192.168.1.42:8000")), Locality::ThisMachine);
}

void TestBackendUrlLocality::anAddressThisMachineLacksIsRefused()
{
    // The only thing worth refusing over: a literal address the kernel does not
    // hold. No bind to it can succeed here, so starting a server is pointless.
    QCOMPARE(locality(QStringLiteral("http://192.168.1.43:8000")), Locality::NotThisMachine);
    QCOMPARE(locality(QStringLiteral("http://10.0.0.7:8000")), Locality::NotThisMachine);
}

void TestBackendUrlLocality::theMachinesOwnNameIsThisMachine()
{
    QCOMPARE(locality(QStringLiteral("http://fabios-mac.local:8000")), Locality::ThisMachine);
    // Case is not part of a host name.
    QCOMPARE(locality(QStringLiteral("http://Fabios-Mac.local:8000")), Locality::ThisMachine);
}

void TestBackendUrlLocality::theShortFormOfASuffixedHostnameIsThisMachine()
{
    // macOS reports the hostname already suffixed ("fabios-mac.local"), so a
    // derivation that only went short -> .local never accepted the bare label
    // the user is just as likely to have typed.
    QCOMPARE(locality(QStringLiteral("http://fabios-mac:8000")), Locality::ThisMachine);
}

void TestBackendUrlLocality::anUnfamiliarNameIsNeverRefused()
{
    // The important one, and the reason Unknown exists as a third answer. A
    // machine called `visivo01` is routinely reached as `visivo01.inaf.it`, or
    // by an /etc/hosts alias, and uvicorn binds those happily. Reading the
    // spelling and declaring them remote would refuse to start a backend that
    // has always started — so an unrecognised name is never a refusal.
    QCOMPARE(locality(QStringLiteral("http://visivo01.inaf.it:8000")), Locality::Unknown);
    QCOMPARE(locality(QStringLiteral("http://my-alias:8000")), Locality::Unknown);
    // Including one that merely looks like ours but is not: claiming it would
    // be just as much a guess as refusing it.
    QCOMPARE(locality(QStringLiteral("http://fabios-mac.inaf.it:8000")), Locality::Unknown);
}

void TestBackendUrlLocality::anEmptyHostFallsBackToLocal()
{
    // A field someone half-filled. The caller's default is loopback, so this
    // must not become a refusal to start anything.
    QCOMPARE(locality(QStringLiteral("")), Locality::ThisMachine);
    QCOMPARE(locality(QStringLiteral("   ")), Locality::ThisMachine);
}

void TestBackendUrlLocality::textThatIsNotAUrlIsNamedAsSuch()
{
    // QUrl reads "localhost:9000" as a scheme plus a path, so the host comes
    // back empty — and taken as "empty means local" the client bound
    // 127.0.0.1:8000, polled an address that resolves to nothing, timed out
    // after thirty seconds, and left an orphaned server to make the next
    // attempt fail with "address already in use".
    QCOMPARE(locality(QStringLiteral("localhost:9000")), Locality::Malformed);
    QCOMPARE(locality(QStringLiteral("192.168.1.42:9000")), Locality::Malformed);
    QCOMPARE(locality(QStringLiteral("not a url at all")), Locality::Malformed);
    // But a real URL still is one.
    QCOMPARE(locality(QStringLiteral("http://localhost:9000")), Locality::ThisMachine);
}

void TestBackendUrlLocality::aV4AddressMatchesItsMappedV6Form()
{
    // The same interface can be reported as 192.168.1.42 or ::ffff:192.168.1.42
    // depending on who is asking; both are this machine.
    QCOMPARE(BackendUrl::localityOf(QStringLiteral("http://192.168.1.42:8000"),
                                    { QHostAddress(QStringLiteral("::ffff:192.168.1.42")) }, {}),
             Locality::ThisMachine);
}

void TestBackendUrlLocality::theHostAndThePortComeFromTheSameParse()
{
    // They did not: hostOf() trimmed and QUrl::port() did not, so a setting
    // stored with a trailing space bound uvicorn to the right host on the
    // default port while the client polled the one the user had asked for, and
    // start-up died on the thirty-second timeout with nothing to explain it.
    const QString padded = QStringLiteral("http://127.0.0.1:9000 ");
    QCOMPARE(BackendUrl::hostOf(padded), QStringLiteral("127.0.0.1"));
    QCOMPARE(BackendUrl::portOf(padded, 8000), 9000);
    QCOMPARE(BackendUrl::portOf(QStringLiteral("http://127.0.0.1"), 8000), 8000);
}
