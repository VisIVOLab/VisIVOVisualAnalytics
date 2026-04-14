#include "MainWindow.h"
#include "Version.h"
#include "app/BackendLauncher.h"
#include "gui/StartupDialog.h"
#include "theme/VisivoTheme.h"

#include <QVTKOpenGLNativeWidget.h>

#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QSurfaceFormat>
#include <QWebEngineUrlScheme>

#include <clocale>
#include <vtkWindow.h>

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    // Register custom URL scheme needed for authentication
    QWebEngineUrlScheme scheme("vlva"_ba);
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    scheme.setDefaultPort(QWebEngineUrlScheme::PortUnspecified);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme);
    QWebEngineUrlScheme::registerScheme(scheme);

    // Enable OpenGL shared contexts
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // Set OpenGL profile needed for VTK
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

    QApplication app(argc, argv);

    // Set locale to C.UTF-8
    std::setlocale(LC_NUMERIC, "C.UTF-8");
    QLocale::setDefault(QLocale::c());

    // Set application info
    QApplication::setApplicationName(u"VisIVO Visual Analytics"_s);
    QApplication::setApplicationVersion(QStringLiteral(VISIVO_VERSION_STR));
    QApplication::setWindowIcon(QIcon(":/icons/VisIVO_512.png"));
    QApplication::setOrganizationName(u"Osservatorio Astrofisico di Catania"_s);
    QApplication::setOrganizationDomain(u"it.inaf.oact"_s);

    // Apply Digital Observatory design system — must run after QApplication is
    // constructed but before any window is created so palette + stylesheet are
    // inherited by all top-level widgets.
    VisivoTheme::applyTo(&app);

    // Customize QDebug message format
    qSetMessagePattern("%{time} "
                       "%{if-debug}D %{endif}"
                       "%{if-info}I %{endif}"
                       "%{if-warning}W %{endif}"
                       "%{if-critical}C %{endif}"
                       "%{if-fatal}F %{endif}"
#ifndef NDEBUG
                       "%{file} %{function}:%{line} "
#endif
                       "%{if-category}%{category} %{endif}%{message}");

    // ── Backend auto-start suppression ────────────────────────────────────────
    // Honour either --no-backend-autostart CLI flag or VISIVO_NO_BACKEND_AUTOSTART
    // env var so that developers / CI / users with an external backend can skip
    // the launch step and connect to an already-running instance.
    const QStringList cliArgs = QCoreApplication::arguments();
    const bool noAutostart =
            cliArgs.contains(u"--no-backend-autostart"_s)
            || !qEnvironmentVariable("VISIVO_NO_BACKEND_AUTOSTART").isEmpty();

    // ── Backend launcher (lives for the full application lifetime) ────────────
    // Owned here in main() so the backend process is terminated cleanly when
    // the application exits.
    BackendLauncher launcher;

    // ── Startup dialog ────────────────────────────────────────────────────────
    // Drives the backend + auth sequence before the main window is shown.
    // Creates MainWindow only after everything is ready.
    StartupDialog startup(&launcher, noAutostart);
    if (startup.exec() != QDialog::Accepted) {
        // User clicked Quit or startup failed unrecoverably.
        return 0;
    }

    // ── Main window ───────────────────────────────────────────────────────────
    MainWindow w;
    w.show();

    return QApplication::exec();
}
