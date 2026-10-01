#pragma once
// TestBackendUrlLocality.h
// QTest class for "is this backend URL served by the machine we run on?", the
// question that decides whether the client may refuse to start a backend.
// Pure string/address logic — no widgets, no VTK, and no network: the machine
// is supplied to the function under test.

#include <QObject>

class TestBackendUrlLocality : public QObject
{
    Q_OBJECT

private slots:
    void loopbackIsThisMachine();
    void theWildcardBindIsThisMachine();
    void anAddressThisMachineHoldsIsThisMachine();
    void anAddressThisMachineLacksIsRefused();
    void theMachinesOwnNameIsThisMachine();
    void theShortFormOfASuffixedHostnameIsThisMachine();
    void anUnfamiliarNameIsNeverRefused();
    void anEmptyHostFallsBackToLocal();
    void textThatIsNotAUrlIsNamedAsSuch();
    void aV4AddressMatchesItsMappedV6Form();
    void theHostAndThePortComeFromTheSameParse();
};
