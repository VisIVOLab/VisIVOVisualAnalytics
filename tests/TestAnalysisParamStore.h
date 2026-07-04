#ifndef TestAnalysisParamStore_h
#define TestAnalysisParamStore_h

#include <QObject>

/// Unit tests for AnalysisParamStore: per-dataset persistence of analysis-tool
/// parameters into a (temporary) ini file. No GUI, no backend.
class TestAnalysisParamStore : public QObject
{
    Q_OBJECT
private slots:
    void init();          // fresh temp ini per test
    void cleanup();

    void roundTrip_savesAndLoads();
    void missing_returnsEmpty();
    void datasetsAreIsolated();
    void groupsAreIsolated();
    void resaveReplacesStaleKeys();
    void emptyKeyOrGroup_noop();

private:
    QString m_iniPath;
};

#endif  // TestAnalysisParamStore_h
