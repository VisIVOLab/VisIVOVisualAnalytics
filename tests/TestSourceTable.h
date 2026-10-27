#pragma once
// TestSourceTable.h
// QTest class for vlkb/SourceTable — the logic of "VLKB cutouts for a source
// table": reading the TSV, choosing columns, parsing the inventory, choosing
// the best dataset and the coverage of each survey. Core-only, no network.

#include <QObject>

class TestSourceTable : public QObject
{
    Q_OBJECT

private slots:
    void parse_readsTheLegacyExample();
    void parse_toleratesCrlfBlankLinesAndRaggedRows();
    void parse_refusesFilesThatAreNotSourceTables();
    void parse_detectsTheSeparator();
    void guessColumn_findsCommonNames();
    void pickSources_skipsBadCoordinatesAndNamesAnonymousRows();
    void parseInventory_keepsCuttableDatasetsOnly();
    void bestEntry_prefersFullOverlap();
    void coverage_comparesWithWhatTheTableFound();
};
