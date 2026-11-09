#pragma once
// TestSystemQuitRedirect.h
// QTest class for SystemQuitRedirect.h: only the platform's own Quit is taken
// from Qt and sent the Quit menu's way.

#include <QObject>

class TestSystemQuitRedirect : public QObject
{
    Q_OBJECT

private slots:
    void onlyASpontaneousQuitIsRedirected();
    void aQuitTheProgramSendsItselfIsLeftToQt();
};
