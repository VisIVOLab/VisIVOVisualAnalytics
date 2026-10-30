#ifndef TestSkavaClient_h
#define TestSkavaClient_h

#include <QObject>

/// Parsers of the SKAVA client against REAL SKAVA responses: the fixtures in
/// tests/data/skava/ were produced by running the SKAVA service in-process on
/// its demo seed (seed_d22), so a field renamed on the server breaks here.
class TestSkavaClient : public QObject
{
    Q_OBJECT
private slots:
    void parsesDiscoverySearch();
    void parsesDatalink();
    void nullScoresStayNaN();
    void parsesErrorEnvelope();
    void explainsAnUnreachableNode();
    void accessUrlForNode();
    // VO protocol (TAP/ObsCore + DataLink VOTable)
    void adqlFromFilter();
    void adqlQuotesStrings();
    void parsesObscoreVotable();
    void parsesDatalinkVotable();
    void datalinkVotableRoutesLikeJson();
    void votableErrorsAreReported();
    void votableSkipsMetaResource();
    void votableStatusRowsMatchOnPath();
    void votablePrefersFirstSyncCutout();
    void adqlOrdersWhenPaging();
    void resolveAccessUsesTheRow();
    // The routing decision and the node services (testbed fixtures)
    void parsesRoutingDecisionJson();
    void parsesNodeServicesJson();
    void parsesNodeServicesVotable();
    void nodeServiceKinds();
    void routesCandidatesInOrder();
    void preferredNodeGoesFirst();
    // The node's SODA job and the Computing Broker's job, as the desktop reads them
    void sodaJobJson();
    void sodaServiceUrlOnBackend();
    void brokerJobJson();
    // Sign-in: the 401 challenge and the IAM's discovery document
    void authChallenge();
    void oidcDocument();
};

#endif  // TestSkavaClient_h
