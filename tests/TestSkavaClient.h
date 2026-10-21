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
    void accessUrlForNode();
    // VO protocol (TAP/ObsCore + DataLink VOTable)
    void adqlFromFilter();
    void adqlQuotesStrings();
    void parsesObscoreVotable();
    void parsesDatalinkVotable();
    void datalinkVotableRoutesLikeJson();
    void votableErrorsAreReported();
    void votableSkipsMetaResource();
    void adqlOrdersWhenPaging();
    void resolveAccessUsesTheRow();
};

#endif  // TestSkavaClient_h
