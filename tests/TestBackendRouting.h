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
};

#endif  // TestBackendRouting_h
