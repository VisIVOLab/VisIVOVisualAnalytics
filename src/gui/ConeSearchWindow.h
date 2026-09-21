#ifndef ConeSearchWindow_h
#define ConeSearchWindow_h

// ConeSearchWindow.h
// Ask a VO Simple Cone Search service what is around a position.
//
// Ported from the legacy client's SimpleConeSearchForm (ViaLactea Visual
// Analytics 1.7.4), which ran a bundled pyvo script through QProcess using
// whichever Python the user had configured, then split the output on commas.
// The search is a backend route now (app/cone_search.py), which also saves the
// whole result as a CSV — so the button the legacy left commented out, "do
// something with these sources", is simply *Open as Catalogue*.

#include "BackendClient.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class ConeSearchWindow : public QWidget
{
    Q_OBJECT

public:
    ConeSearchWindow(const QString &backendUrl, const QString &backendToken,
                     QWidget *parent = nullptr);
    ~ConeSearchWindow() override;

    /// Start from a position the user is already looking at.
    void setPosition(double ra, double dec, double radiusDeg);

signals:
    /// The saved result, ready to open in the 3-D catalogue viewer.
    void catalogueReady(const QString &backendPath);

private slots:
    void search();
    void onServicesFinished();
    void onSearchFinished();

private:
    void buildUi();

    QString m_backendUrl;
    QString m_backendToken;
    QString m_savedPath;

    QFutureWatcher<QList<BackendConeSearchService>> m_servicesWatcher;
    QFutureWatcher<BackendConeSearchResult> m_searchWatcher;

    QPointer<QComboBox> m_service;
    QPointer<QDoubleSpinBox> m_ra;
    QPointer<QDoubleSpinBox> m_dec;
    QPointer<QDoubleSpinBox> m_radius;
    QPointer<QComboBox> m_verbosity;
    QPointer<QTableWidget> m_table;
    QPointer<QLabel> m_status;
    QPointer<QPushButton> m_searchButton;
    QPointer<QPushButton> m_openButton;
};

#endif   // ConeSearchWindow_h
