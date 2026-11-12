#pragma once
// TestBackendProcessTree.h
// QTest class for the local backend's process tree (app/BackendProcessTree.h,
// issue #176): real child processes started through the same helper the
// launcher uses — a shell standing in for uvicorn, a `sleep` it spawns for a
// worker — and the quit path, which must leave none of them behind.

#include <QObject>

class TestBackendProcessTree : public QObject
{
    Q_OBJECT

private slots:
    void backendLeadsItsOwnGroup();
    void stopEndsTheWholeTreeAfterAskingFirst();
    void stopKillsAWorkerThatIgnoresTerm();
    void stopEndsWorkersTheServerLeftBehind();
    void stopWithNoTreeIsHarmless();

    // RetiredProcessTrees
    void retiredTreeEndsWithoutTouchingTheCurrentOne();
    void retiringAnEmptyTreeKeepsNothing();
    void quitEndsRetiredTrees();
};
