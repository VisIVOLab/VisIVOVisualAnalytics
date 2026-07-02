// tests/main.cpp
// Combined QTest runner for VisIVO Visual Analytics unit tests.
// Runs all test classes in sequence; returns non-zero if any test fails.
// QCoreApplication is used — no GUI, no display required.

#include "TestBackendClient.h"
#include "TestCatalogueParser.h"
#include "TestBackendRouting.h"
#include "TestImageLod.h"

#include <QCoreApplication>
#include <QTest>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    int status = 0;
    { TestBackendClient   t; status |= QTest::qExec(&t, argc, argv); }
    { TestCatalogueParser t; status |= QTest::qExec(&t, argc, argv); }
    { TestBackendRouting  t; status |= QTest::qExec(&t, argc, argv); }
    { TestImageLod        t; status |= QTest::qExec(&t, argc, argv); }
    return status;
}
