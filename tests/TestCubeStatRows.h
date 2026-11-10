#pragma once
// TestCubeStatRows.h
// QTest class for what the Inspector's "3D cube" statistic rows say, the
// preview of a cube above the full-resolution threshold included (#150).
// Header-only logic; no window.

#include <QObject>

class TestCubeStatRows : public QObject
{
    Q_OBJECT

private slots:
    void thePreviewShowsNoMeanOrRmsUntilTheBackendAnswers();
    void theBackendScanWinsOverAnyLoadedGrid();
    void aFullResolutionGridShowsItsOwnNumbers();
    void aCameraRoiIsNotTheWholeCube();
    void aFailedScanSaysWhy();
};
