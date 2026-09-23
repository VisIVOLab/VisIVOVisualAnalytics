#ifndef HiPSSurveyBrowserDialog_h
#define HiPSSurveyBrowserDialog_h

#include "app/BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// ---------------------------------------------------------------------------
// HiPSSurveyBrowserDialog — pick a survey from the CDS registry.
//
// The viewer used to accept nothing but a typed URL, which meant you had to
// know one already. This lists every image HiPS the MOCServer knows about,
// grouped by the category its publisher gave it, and filtered as you type.
// ---------------------------------------------------------------------------

class HiPSSurveyBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    HiPSSurveyBrowserDialog(const QString &backendUrl, const QString &backendToken,
                            QWidget *parent = nullptr);

    // The chosen survey's base URL, empty when nothing was picked.
    QString selectedUrl() const { return m_selectedUrl; }
    QString selectedTitle() const { return m_selectedTitle; }

private slots:
    void onLoaded();
    void onFilterChanged(const QString &text);
    void onSelectionChanged();
    void onItemActivated(QTreeWidgetItem *item, int column);
    void onRefresh();

private:
    void startLoad(bool refresh);
    void populate();

    QString m_backendUrl;
    QString m_backendToken;
    QString m_selectedUrl;
    QString m_selectedTitle;

    QVector<BackendHiPSRegistryEntry> m_surveys;

    QPointer<QLineEdit>   m_filterEdit;
    QPointer<QTreeWidget> m_tree;
    QPointer<QLabel>      m_statusLabel;
    QPointer<QPushButton> m_openButton;
    QPointer<QPushButton> m_refreshButton;

    QFutureWatcher<BackendHiPSRegistryResult> m_watcher;
};

#endif // HiPSSurveyBrowserDialog_h
