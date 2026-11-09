#include "TestSystemQuitRedirect.h"

#include "gui/SystemQuitRedirect.h"

#include <QCoreApplication>
#include <QtTest>

void TestSystemQuitRedirect::onlyASpontaneousQuitIsRedirected()
{
    // The Dock's Quit reaches the application as a spontaneous Quit.
    QVERIFY(visivo::redirectsSystemQuit(QEvent::Quit, /*spontaneous=*/true));
    QVERIFY(!visivo::redirectsSystemQuit(QEvent::Quit, /*spontaneous=*/false));
    QVERIFY(!visivo::redirectsSystemQuit(QEvent::Close, /*spontaneous=*/true));
}

void TestSystemQuitRedirect::aQuitTheProgramSendsItselfIsLeftToQt()
{
    // A Quit sent by the program (not spontaneous) passes the filter untouched
    // and runs no question. (A spontaneous one cannot be built from outside
    // Qt's private API; redirectsSystemQuit above is that decision.)
    int asked = 0;
    visivo::SystemQuitRedirect redirect([&asked]() { ++asked; });
    QCoreApplication::instance()->installEventFilter(&redirect);
    QObject target;
    QEvent quit(QEvent::Quit);
    QCoreApplication::sendEvent(&target, &quit);   // not the application object either
    QCoreApplication::processEvents();
    QCOMPARE(asked, 0);
    QCoreApplication::instance()->removeEventFilter(&redirect);
}
