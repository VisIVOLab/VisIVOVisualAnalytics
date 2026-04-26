#ifndef HiPSWindow_h
#define HiPSWindow_h

#include "HiPSViewportWidget.h"
#include "app/BackendClient.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QWidget>

class QAction;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QToolButton;

struct HiPSOpenTaskResult
{
    bool valid{ false };
    QString error;
    BackendHiPSSurveyInfo survey;
};

// ---------------------------------------------------------------------------
// HiPSWindow  —  Milestone 2
//
// Layout (QVBoxLayout):
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ Strip 1  [Survey URL ___________] [Open]  │[-][+][⌂] N  │[⊞Grid][⊠Cat]│
// ├─────────────────────────────────────────────────────────────────────────┤
// │ Strip 2  [Target: ___________] [Go]  │ [2nd survey URL] [Open2] [α──]  │
// ├─────────────────────────────────────────────────────────────────────────┤
// │                  HiPSViewportWidget                                      │
// ├─────────────────────────────────────────────────────────────────────────┤
// │ Status   message  │  RA  Dec  │  Survey  │  FOV  │  Order               │
// └─────────────────────────────────────────────────────────────────────────┘
// ---------------------------------------------------------------------------
class HiPSWindow : public QWidget
{
    Q_OBJECT

public:
    explicit HiPSWindow(const QString &backendUrl, const QString &backendToken,
                        QWidget *parent = nullptr);
    ~HiPSWindow() override = default;

    void openSurveyUrl(const QString &url);
    void setCenter(double raDeg, double decDeg);

private slots:
    // Primary survey
    void onOpenClicked();
    void onZoomIn();
    void onZoomOut();
    void onResetView();
    void onCursorCoordinates(double ra, double dec);
    void onCenterChanged(double ra, double dec);
    void onFovChanged(double fov);
    void onOrderChanged(int order);
    void onLoadingChanged(bool loading);

    // Task 1 – Grid
    void onGridToggled(bool checked);

    // Task 2 – Secondary survey
    void onOpenSecondaryClicked();
    void onOpacityChanged(int value);
    void onBlendModeToggled(bool sideBySide);

    // Task 3 – Catalogue overlay
    void onLoadCatalogueClicked();
    void onCatalogueOverlayRefreshed(int totalInView, bool truncated);

    // Task 4 – Target resolver
    void onResolveClicked();
    void onResolveFinished();

private:
    void setupUi();
    void connectViewport();
    void beginOpenSurvey(const QString &url, bool secondary = false);
    void finishOpenSurvey();
    void finishOpenSurveySec();
    void showStatus(const QString &msg);
    void clearStatus();
    QString fmtRa(double ra) const;
    QString fmtDec(double dec) const;

    QString m_backendUrl;
    QString m_backendToken;
    BackendHiPSSurveyInfo m_currentSurvey;
    BackendHiPSSurveyInfo m_currentSurveySec;

    // ── Widgets ───────────────────────────────────────────────────────────
    QPointer<HiPSViewportWidget> m_viewport;

    // Strip 1 – primary survey + overlays
    QPointer<QLineEdit>   m_urlEdit;
    QPointer<QPushButton> m_openBtn;
    QPointer<QToolButton> m_zoomInBtn;
    QPointer<QToolButton> m_zoomOutBtn;
    QPointer<QToolButton> m_resetBtn;
    QPointer<QLabel>      m_orderLabel;
    QPointer<QToolButton> m_gridBtn;
    QPointer<QToolButton> m_catBtn;

    // Strip 2 – target resolver + secondary survey
    QPointer<QLineEdit>   m_targetEdit;
    QPointer<QPushButton> m_resolveBtn;
    QPointer<QLineEdit>   m_urlSecEdit;
    QPointer<QPushButton> m_openSecBtn;
    QPointer<QSlider>     m_opacitySlider;
    QPointer<QToolButton> m_sideBySideBtn;

    // Status strip
    QPointer<QLabel> m_coordsLabel;
    QPointer<QLabel> m_surveyLabel;
    QPointer<QLabel> m_fovLabel;
    QPointer<QLabel> m_statusMsgLabel;

    // ── Async watchers ────────────────────────────────────────────────────
    QFutureWatcher<HiPSOpenTaskResult> m_openWatcher;
    QFutureWatcher<HiPSOpenTaskResult> m_openWatcherSec;
    QFutureWatcher<BackendTargetResolveResult> m_resolveWatcher;
};

#endif // HiPSWindow_h
