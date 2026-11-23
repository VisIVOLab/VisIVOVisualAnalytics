#ifndef TestDiagnosticsTask_h
#define TestDiagnosticsTask_h

#include <QObject>

class TestDiagnosticsTask : public QObject
{
    Q_OBJECT

private slots:
    void theTaskIsCreatedPolledAndGivesTheSameResult();
    void progressReachesTheControl();
    void cancelSendsDeleteToTheTask();
    void aRefusedRequestComesBackAsAnError();
    void aTaskThatFailsIsAnErrorNotARetry();
    void anOlderBackendFallsBackToTheSynchronousRoute();
};

#endif
