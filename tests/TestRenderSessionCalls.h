#pragma once
// TestRenderSessionCalls.h
// QTest class for the requests that give back a render session or a place in
// the backend's render queue (issue #4), against a one-shot local HTTP server:
// what goes on the wire when a viewer waiting in the queue closes.

#include <QObject>

class TestRenderSessionCalls : public QObject
{
    Q_OBJECT

private slots:
    void closingWhileQueuedGivesThePlaceBack();
    void aLateSessionIsDeleted();
    void nothingToGiveBackSendsNothing();
    void theRequestsOutliveTheViewer();
};
