#ifndef DataHubWidget_h
#define DataHubWidget_h

#include <QWidget>
#include <QPointer>

class QLabel;
class QPushButton;
class QListWidget;
class QFrame;

// ---------------------------------------------------------------------------
// DataHubWidget
//
// Central landing page for VisIVO Visual Analytics.
// Provides backend status, quick-action buttons for all dataset types, and a
// placeholder recent-datasets list.  All actions are emitted as signals so
// that MainWindow owns the business logic.
// ---------------------------------------------------------------------------
class DataHubWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DataHubWidget(QWidget *parent = nullptr);

    // Called by MainWindow whenever settings change or the tab becomes active.
    void setBackendUrl(const QString &url, const QString &token);

public slots:
    // Triggers a synchronous health-check against the configured backend URL
    // and updates the status indicator.
    void refreshStatus();

signals:
    void openDataRequested();
    void openCatalogueRequested();
    void openVbtRequested();
    void openHiPSRequested();

private:
    void buildUi();

    QString backendUrl;
    QString backendToken;

    QPointer<QLabel>       labelBackendUrl;
    QPointer<QLabel>       labelStatusIndicator;
    QPointer<QLabel>       labelStatusText;
    QPointer<QPushButton>  btnCheckConnection;
    QPointer<QPushButton>  btnOpenData;
    QPointer<QPushButton>  btnOpenCatalogue;
    QPointer<QPushButton>  btnOpenVbt;
    QPointer<QPushButton>  btnOpenHiPS;
    QPointer<QListWidget>  listRecentDatasets;
};

#endif // DataHubWidget_h
