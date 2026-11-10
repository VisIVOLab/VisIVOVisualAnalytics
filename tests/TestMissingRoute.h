#pragma once
// TestMissingRoute.h
// QTest class for the words of a backend older than this client
// (app/MissingRoute.h): a route that is not there told apart from a route's
// own 404, and the line that names the backend, read off real replies of a
// loopback server as the ray-traced views read them.

#include <QObject>

class TestMissingRoute : public QObject
{
    Q_OBJECT

private slots:
    void aRoutesOwn404IsNotAMissingRoute();
    void fastApisBare404IsAMissingRoute();
    void messageNamesTheBackend();
    void replyOfAnOldBackendNamesIt();
};
