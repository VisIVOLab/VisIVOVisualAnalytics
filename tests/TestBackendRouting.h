#ifndef TestBackendRouting_h
#define TestBackendRouting_h

#include <QObject>

/// Unit tests for the distributed-backend desktop layer: the Settings
/// multi-backend registry and the SKAVA → backend routing decision
/// (pickBackendForSkavaDataset). Pure logic, no GUI / VTK / live network.
class TestBackendRouting : public QObject
{
    Q_OBJECT
private slots:
    // Settings registry
    void registryAlwaysHasLocal();
    void sessionBackendIsUsedButNeverSaved();
    void sessionBackendNotWrittenIntoTheRegistry();
    void launchOptions_localUnlessABackendIsGiven();
    void launchOptions_optionsWithoutValueAreErrors();
    void launchOptions_tokenFromFileOrEnvironment();
    void cluster_launcherOutputIsParsed();
    void cluster_remoteCommandLineQuotesEveryArgument();
    void cluster_localPortAvoidsTheLocalBackend();
    void cluster_tokenNeverShown();
    void cluster_cancelByIdOrByName();
    void cluster_profileCannotInjectSshOptions();
    void cluster_profilesRoundTrip();
    void launchOptions_connectCluster();
    void upsertInsertsAndUpdates();
    void removeDropsNode();
    void localCannotBeRemoved();
    void defaultBackendIdFallsBackToLocal();

    // Routing decision
    void routeByExactUrl();
    void routeByUrlIgnoresTrailingSlash();
    void routeByNodeCode();
    void routeNoMatchReturnsEmpty();
    void routeEmptyDatalinkReturnsEmpty();
    void routeNullSettingsReturnsEmpty();
    void routeUsesTheChosenNodesReplica();
    void routePrefersRankingOrder();
    void routeSkipsUnsupportedKind();
    void routeSkipsAuthWithoutToken();
    void routeSkipsNodeWithoutReplica();
    void kindMapping();
    void routeTreatsLoopbackNamesAsOneHost();
    void findRegisteredBackendMatchesLikeRouting();
};

#endif  // TestBackendRouting_h
