#pragma once
// TestRemoteLevels.h
// QTest class for the remote view's starting values (issue #82): reading the
// backend's threshold and range, and the threshold slider's bounds.
// Header-only logic; no window.

#include <QObject>

class TestRemoteLevels : public QObject
{
    Q_OBJECT

private slots:
    void createSessionAnswerIsRead();
    void levelsAnswerIsRead();
    void unknownValuesStayUnknown();
    void theRemoteRangeWidensThePreviewsBounds();
    void theBackendsThresholdIsNeverClamped();
    void sliderPositionStaysInRange();
};
