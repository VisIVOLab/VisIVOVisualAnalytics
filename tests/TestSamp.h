#pragma once
// TestSamp.h
// QTest class for the SAMP client's logic (issue #130): the XML-RPC codec, the
// HTTP request parsing and the live loopback callback server, the decoding of
// samp.client.* callbacks and the replies to calls, row indices, table
// identity and the original-row ↔ loaded-entry mapping.

#include <QObject>

class TestSamp : public QObject
{
    Q_OBJECT

private slots:
    // XML-RPC
    void xmlRpc_callRoundTripsNestedValuesAndEscaping();
    void xmlRpc_parsesImplicitStringsAndWhitespace();
    void xmlRpc_responseAndFault();
    void xmlRpc_refusesMalformedBodies();
    void xmlRpc_refusesDeepNesting();

    // HTTP
    void http_parsesCompleteAndPartialRequests();
    void http_refusesWhatAHubNeverSends();
    void callbackServer_answersALoopbackPost();

    // Callback dispatch
    void decode_notificationCallAndResponse();
    void decode_refusesBadKeyArityAndMethod();
    void validate_andResponseForCalls();
    void lockFile_parsesKeysAndIgnoresComments();
    void pendingCalls_reusedMsgIdAfterReconnect();

    // Rows and tables
    void rows_parseIndicesAndLists();
    void rows_buildHighlightAndSelectParams();
    void registry_matchesByTableIdOrUrl();
    void rowMap_mapsThroughTheOriginalRowOrder();
    void stagedFileName_addsTheExtensionTheMTypeImplies();
    void parser_keepsSourceRowsWhenRowsAreSkipped();
    void registry_listsEveryIdentityOfACatalogue();
    void catalogueLoadMType_followsTheBackendsChoice();
    void parser_namesUnnamedSourcesByTheirTableRow();
    void reconnect_aLateHubLossDoesNotUndoDisconnect();
    void pendingTables_resolveRowMessagesBeforeTheSendReturns();
    void reconnect_keepsTryingAfterAFailedConnect();
    void hubStatusKey_ignoresOrderAndSeesChanges();
    void receivedCopies_showTheSendersName();
    void toolTip_breaksLongValuesAndLeavesShortOnes();

    // Viewer actions: recipients, subsets, positions
    void recipients_followSubscriptionsAndWildcards();
    void subset_mapsRowsBothWays();
    void rowMap_turnsASelectionIntoSortedRows();
    void pointAt_paramsAreDegreesInRange();
    void skyPosition_framesRotateToIcrs();
    void skyPosition_pixelsProjectAsTheHeaderSays();
    void backendAcks_deduplicateAcknowledgeAndGiveUp();
    void pendingTables_forgetFailedAndStaleSends();
    void skyPosition_fallbackHandlesSipSwapAndSign();
};
