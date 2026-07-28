#ifndef INSPECTORPANEL_H
#define INSPECTORPANEL_H

// InspectorPanel — the right-hand contextual inspector *shell* of Design 1d
// (tappa 3). Three tabs (Properties / Analysis / Provenance) plus a cloud chip
// and a compact pane-layout + Linked strip. This is only the SHELL: the Analysis
// tab lists the existing tools as buttons that trigger the current QActions
// (which still open today's dialogs). Migrating those dialogs into inline task
// forms is tappa 4 and deliberately NOT done here.

#include <QWidget>
#include <QVariantMap>
#include <QHash>

class QLabel;
class QTabWidget;
class QVBoxLayout;
class QAction;
class QToolButton;

class InspectorPanel : public QWidget
{
    Q_OBJECT
public:
    explicit InspectorPanel(QWidget *parent = nullptr);

    /// The live cloud/remote chip at the top (updated by WorkspaceChrome poll).
    QLabel *cloudChip() const { return m_cloudChip; }

    /// Properties tab: replace the key/value rows shown.
    void setProperties(const QList<QPair<QString, QString>> &rows);

    /// Properties tab: append a full widget section (e.g. a migrated sidebar page
    /// such as the 3D/2D view settings) below the key/value rows. Ownership moves
    /// to the Properties page.
    void addPropertiesSection(QWidget *section);

    /// Analysis tab: add a captioned group, then tools under it. Each tool is a
    /// button that triggers @p action (the existing dialog-opening QAction).
    void addToolGroup(const QString &caption);
    void addTool(const QString &groupCaption, const QString &label, QAction *action);

    /// Provenance tab: show the recipe (params) of the selected product.
    void showProvenance(const QString &title, const QVariantMap &params);
    void clearProvenance();

    /// Pane-layout + Linked strip. Windows enable what they support.
    void setPaneLayoutEnabled(bool enabled);
    void setLinkedSupported(bool supported);
    void setLinkedChecked(bool checked);

signals:
    void paneLayoutRequested(int panes);   ///< 1 / 2 / 4
    void linkedToggled(bool on);

private:
    QVBoxLayout *groupLayout(const QString &caption);   ///< find/create an analysis group box

    QLabel *m_cloudChip = nullptr;
    QTabWidget *m_tabs = nullptr;

    QWidget *m_propsPage = nullptr;
    QVBoxLayout *m_propsLayout = nullptr;

    QWidget *m_analysisPage = nullptr;
    QVBoxLayout *m_analysisLayout = nullptr;
    QHash<QString, QVBoxLayout *> m_groupLayouts;

    QWidget *m_provPage = nullptr;
    QVBoxLayout *m_provLayout = nullptr;
    QLabel *m_provTitle = nullptr;
    QLabel *m_provBody = nullptr;
    QVariantMap m_provParams;

    QToolButton *m_linkedBtn = nullptr;
    QToolButton *m_pane1 = nullptr;
    QToolButton *m_pane2 = nullptr;
    QToolButton *m_pane4 = nullptr;
};

#endif // INSPECTORPANEL_H
