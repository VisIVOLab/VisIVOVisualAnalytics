#pragma once
// TestProbeSpectrumFetch.h
// QTest class for the live spectrum fetched from the backend on a preview
// (ProbeSpectrumFetch.h): one request in flight, the latest pixel next, a
// stale reply never shown; and the preview-to-cube voxel mapping.

#include <QObject>

class TestProbeSpectrumFetch : public QObject
{
    Q_OBJECT

private slots:
    void aStillCursorIsFetchedOnce();
    void movesWhileInFlightCollapseToTheLatest();
    void aReplyForAPixelLeftBehindIsNotShown();
    void backToTheSamePixelIsNotFetchedAgain();
    void aResetDropsTheReplyInFlight();
    void aProbeSwitchedOffShowsNothing();
    void previewVoxelsAreStridedCubeVoxels();
};
