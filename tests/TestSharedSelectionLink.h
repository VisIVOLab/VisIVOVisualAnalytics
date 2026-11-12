#pragma once
// TestSharedSelectionLink.h
// The cube viewer's end of the selection hub (issue #188) against a running
// backend: skipped unless VISIVO_SHARE_E2E_URL, VISIVO_SHARE_E2E_TOKEN_FILE and
// VISIVO_SHARE_E2E_CUBE name one, a file holding its token and a cube on it.

#include <QObject>

class TestSharedSelectionLink : public QObject
{
    Q_OBJECT

private slots:
    void aPublishedSelectionReachesTheOtherClientWithItsRecord();
    // Against a fake hub, always run.
    void theDelayedRetryOfAnOlderSelectionIsDropped();
};
