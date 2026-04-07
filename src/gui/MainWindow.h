#ifndef MainWindow_h
#define MainWindow_h

#include <QMainWindow>
#include <QPointer>

#include <memory>

class AuthWrapper;
class DiagnosticsWindow;
class Settings;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    /**
   * Set the application theme
   */
    void setApplicationTheme();

    /**
     * Open a dataset via the backend. The user browses the backend filesystem
     * through RemoteFileBrowserDialog; the file is always registered on the
     * backend before a viewer window is created.
     */
    void openData();

    /**
   * Show a QFileDialog to open a CSV catalogue in the 3D viewer
   */
    void openCatalogue3D();
    void openVbt();

    /**
   * Show the Settings Dialog
   */
    void openSettingsDialog();

    /**
   * Show the About Dialog
   */
    void openAboutDialog();

    /**
   * Reload the Panoramic View
   */
    void loadPanoramicView();

    /**
   * Change the selection mode for the Panoramic View
   */
    void changeViewSelectionMode();

    /**
   * Update the coordinates and the region for the search request
   * @param point Center of selection (comma separated values)
   * @param area Area of selection (comma separated values), empty string if
   * selection = point
   */
    void skyRegionSelected(const QString &point, const QString &area);

private:
    Ui::MainWindow *ui;
    QPointer<Settings> settings;
    QPointer<AuthWrapper> auth;
    QPointer<DiagnosticsWindow> diagnosticsWindow;
    void openDiagnosticsWindow();
};

#endif
