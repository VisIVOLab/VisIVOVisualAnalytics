#include "MainWindow.h"
#include "Version.h"
#include "app/BackendLauncher.h"
#include "app/ThemeManager.h"
#include "gui/StartupDialog.h"

#include <QVTKOpenGLNativeWidget.h>

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QLocale>
#include <QSettings>
#include <QSurfaceFormat>
#include <QWebEngineUrlScheme>

#include <clocale>
#include <vtkOutputWindow.h>
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

    // Route VTK warnings/errors to stderr instead of the GUI popup window.
    // On Windows, VTK's default output sink is vtkWin32OutputWindow — a modal
    // popup that hijacks focus and freezes when many messages arrive in a
    // short window (e.g. the burst of "XR_ERROR_*" diagnostics emitted when
    // an OpenXR session fails to start without a connected HMD). On every
    // platform we just want the messages in the terminal / Qt log, never as
    // a separate window. Must be set before any VTK code touches the global
    // instance, hence here at the very top of main.
    vtkOutputWindow::GetInstance()->SetDisplayModeToAlwaysStdErr();

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

    // Apply theme — must run after QApplication is constructed but before any
    // window is created so palette + stylesheet are inherited by all widgets.
    // Read the persisted scheme directly from QSettings to avoid the full
    // Settings object (which spawns a python process to locate the interpreter).
    {
        const QString workDir  = QDir::home().absoluteFilePath(app.applicationName());
        const QString iniPath  = QDir(workDir).absoluteFilePath(u"Settings.ini"_s);
        QSettings rawSettings(iniPath, QSettings::IniFormat);
        const auto storedInt   = rawSettings.value(u"UI/theme"_s,
                                                     static_cast<int>(Qt::ColorScheme::Unknown))
                                         .toInt();
        const auto scheme      = static_cast<Qt::ColorScheme>(storedInt);
        ThemeManager::instance().apply(scheme, &app);
    }

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
