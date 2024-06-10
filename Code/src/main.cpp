#include <QApplication>
#include <QLocale>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <QWebEngineUrlScheme>
#include <QSurfaceFormat>
#include <clocale>
#include "pqPVApplicationCore.h"

#include "startupwindow.h"

int main(int argc, char *argv[])
{

    QSurfaceFormat glFormat;
    glFormat.setVersion(3, 3);
    glFormat.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(glFormat);

    QApplication a(argc, argv);

    // paraview init
    pqPVApplicationCore appCore(argc, argv);

    a.setApplicationName("VisIVO - Visual Analytics client");
    a.setApplicationVersion("2.0");
    a.setWindowIcon(QIcon(":/icons/logo_256.png"));

    setlocale(LC_NUMERIC, "C");
    QLocale::setDefault(QLocale::c());
    
    StartupWindow *startupWindow = new StartupWindow();
    startupWindow->show();


    return a.exec();
}
