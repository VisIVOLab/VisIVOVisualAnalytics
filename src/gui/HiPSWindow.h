#ifndef HiPSWindow_h
#define HiPSWindow_h

#include "HiPSViewportWidget.h"
#include "app/BackendClient.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

struct HiPSOpenTaskResult
{
    bool valid{ false };
    QString error;
    BackendHiPSSurveyInfo survey;
};

// ---------------------------------------------------------------------------
// HiPSWindow
//
// Embeddable QWidget for the HiPS viewer — designed to live as a closable
// tab in the main window's QTabWidget.
//
// Layout (QVBoxLayout):
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ Tool strip  [Survey URL ______________________] [Open] │ [-][+][⌂]  N  │
// ├─────────────────────────────────────────────────────────────────────────┤
// │                                                                          │
// │              HiPSViewportWidget  (stretch = 1)                           │
// │                                                                          │
// ├─────────────────────────────────────────────────────────────────────────┤
// │ Status strip  RA: HHhMMmSSs  Dec: ±DD°MM'SS"  │  Survey  │  FOV X°     │
// └─────────────────────────────────────────────────────────────────────────┘
// ---------------------------------------------------------------------------
class HiPSWindow : public QWidget
{
    Q_OBJECT

public:
    explicit HiPSWindow(const QString &backendUrl, const QString &backendToken,
                        QWidget *parent = nullptr);
    ~HiPSWindow() override = default;

    // Open a survey from the given URL immediately (called programmatically).
    void openSurveyUrl(const QString &url);

private slots:
    void onOpenClicked();
    void onZoomIn();
    void onZoomOut();
    void onResetView();
    void onCursorCoordinates(double ra, double dec);
    void onCenterChanged(double ra, double dec);
    void onFovChanged(double fov);
    void onOrderChanged(int order);
    void onLoadingChanged(bool loading);

private:
    void setupUi();
    void connectViewport();
    void beginOpenSurvey(const QString &url);
    void finishOpenSurvey();
    void showStatus(const QString &msg);
    void clearStatus();

    QString m_backendUrl;
    QString m_backendToken;
    BackendHiPSSurveyInfo m_currentSurvey;

    // ── Widgets ───────────────────────────────────────────────────────────
    QPointer<HiPSViewportWidget> m_viewport;
    QPointer<QLineEdit>   m_urlEdit;
    QPointer<QPushButton> m_openBtn;
    QPointer<QToolButton> m_zoomInBtn;
    QPointer<QToolButton> m_zoomOutBtn;
    QPointer<QToolButton> m_resetBtn;
    QPointer<QLabel>      m_orderLabel;

    // Status strip labels
    QPointer<QLabel> m_coordsLabel;
    QPointer<QLabel> m_surveyLabel;
    QPointer<QLabel> m_fovLabel;
    QPointer<QLabel> m_statusMsgLabel;

    // ── Async survey open ─────────────────────────────────────────────────
    QFutureWatcher<HiPSOpenTaskResult> m_openWatcher;
};

#endif // HiPSWindow_h
