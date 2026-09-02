#include "MainWindow.h"
#include "Version.h"
#include "app/BackendClient.h"
#include "app/BackendLauncher.h"
#include "app/ThemeManager.h"
#include "gui/StartupDialog.h"

#include <QVTKOpenGLNativeWidget.h>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QUuid>
#include <QLocale>
#include <QSettings>
#include <QSurfaceFormat>
#include <QWebEngineUrlScheme>

#include <clocale>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOutputWindow.h>
#include <vtkWindow.h>

// On hybrid-graphics Windows laptops (NVIDIA Optimus / AMD PowerXpress), the
// driver decides which GPU runs the app by *inspecting these exported symbols
// from the .exe itself*. They must live in the main executable's exports table
// — exporting them from a DLL has no effect. Without this hint, VisIVO falls
// onto the Intel iGPU while the Oculus / SteamVR compositor binds to the
// discrete GPU, and xrCreateSession fails with XR_ERROR_GRAPHICS_DEVICE_INVALID.
#ifdef _WIN32
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {
// VTK's default "output window" on Windows is vtkWin32OutputWindow, a modal
// popup that overrides DisplayText() so SetDisplayMode(...) on the global
// instance has no effect — the popup still appears. The only way to fully
// silence it is to swap the global instance for one that NEVER touches the OS
// GUI. We use a minimal subclass that forwards everything to qWarning(), so
// VTK diagnostics stay visible in stdout/stderr and any Qt log handler the
// app installs catches them along with regular Qt messages.
class VTKToQtOutputWindow : public vtkOutputWindow
{
public:
    vtkTypeMacro(VTKToQtOutputWindow, vtkOutputWindow);
    static VTKToQtOutputWindow *New()
    {
        auto *self = new VTKToQtOutputWindow;
        self->InitializeObjectBase();
        return self;
    }
    void DisplayText(const char *msg) override
    {
        if (msg && *msg) {
            qWarning().noquote() << "[vtk]" << msg;
        }
    }
protected:
    VTKToQtOutputWindow()  = default;
    ~VTKToQtOutputWindow() override = default;
private:
    VTKToQtOutputWindow(const VTKToQtOutputWindow &) = delete;
    void operator=(const VTKToQtOutputWindow &)      = delete;
};
} // namespace

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

    // Replace VTK's global output window with one that forwards to qWarning,
    // bypassing the Windows-only vtkWin32OutputWindow popup (whose own
    // DisplayText override ignores SetDisplayMode, so the more obvious fix
    // doesn't work — see VTKToQtOutputWindow above). Must run BEFORE any
    // VTK code can lazily build the default instance via the factory.
    {
        vtkNew<VTKToQtOutputWindow> qtOut;
        vtkOutputWindow::SetInstance(qtOut);
    }

    QApplication app(argc, argv);

    // Set locale to C.UTF-8
    std::setlocale(LC_NUMERIC, "C.UTF-8");
    QLocale::setDefault(QLocale::c());

    // Give this desktop instance a private backend session so several desktops
    // sharing one backend don't see each other's datasets (or copilot config).
    // A fresh id per launch is enough for isolation; the backend evicts it on idle.
    BackendClient::setDefaultSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces));

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
