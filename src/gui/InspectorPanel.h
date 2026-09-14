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
class QScrollArea;
class QAction;
class QToolButton;
class AssistantPanel;

class InspectorPanel : public QWidget
{
    Q_OBJECT
public:
    explicit InspectorPanel(QWidget *parent = nullptr);

    /// The live cloud/remote chip at the top (updated by WorkspaceChrome poll).
    QLabel *cloudChip() const { return m_cloudChip; }

    /// Properties tab: replace the key/value rows shown.
    void setProperties(const QList<QPair<QString, QString>> &rows);

    // ── Structured Properties builder (identity + captioned sections) ─────────
    /// Clear all Properties rows (call before the add* sequence).
    void clearProperties();
    /// Section caption (Lato 10px/700, 0.14em) — or a lighter sub-caption.
    void addPropertyCaption(const QString &caption, bool sub = false);
    /// Key/value row with a string value (value in mono, right-aligned).
    void addPropertyRow(const QString &key, const QString &value);
    /// Key/value row whose value is a live widget (e.g. a re-hosted stat label).
    void addPropertyRow(const QString &key, QWidget *valueWidget);

    /// Properties tab: append a full widget section (e.g. a migrated sidebar page
    /// such as the 3D/2D view settings) below the key/value rows. Ownership moves
    /// to the Properties page.
    void addPropertiesSection(QWidget *section);

    /// Analysis tab: add a captioned group, then tools under it. Each tool is a
    /// button that triggers @p action (the existing dialog-opening QAction).
    void addToolGroup(const QString &caption);
    void addTool(const QString &groupCaption, const QString &label, QAction *action);

    /// Analysis tab: mount a context section at the TOP of the tab (above the tool groups) —
    /// used for controls that act on the ACTIVE pane (e.g. the migrated Spectrum controls). The
    /// caller owns the widget and toggles its visibility as the active pane changes.
    void addAnalysisSection(QWidget *section);

    /// Parameters tab, top half: the ACTIVE tool's live settings. The caller owns
    /// @p settings (it is re-homed here, never deleted) and calls this when the
    /// tool opens and clearToolSettings() when it closes. The bottom half keeps
    /// showing the selected product's recorded parameters, so one tab answers
    /// both "what am I about to do" and "what was this made with".
    void setToolSettings(const QString &toolName, QWidget *settings);
    void clearToolSettings();
    /// Which tool's settings are mounted, or empty. A tool clears only its OWN
    /// section: turning contours off must not wipe the polarisation vectors'
    /// settings that were mounted after them.
    QString activeToolSettings() const;

    /// Select / query the active tab (0 Properties, 1 Analysis, 2 Parameters).
    /// Used by the collapsible-rail entries and the auto-expand triggers (§4.1.4).
    void showTab(int index);
    int currentTab() const;

    /// Preferred height of the Properties tab content (for a no-scroll check).
    int propertiesContentHeight() const;

signals:
    /// Emitted when an Analysis tool button is triggered — the window uses it to
    /// auto-expand the Inspector onto the Analysis tab (§4.1.4).
    void toolLaunched();

public:

    /// Deferred (post-layout) diagnostic: logs the Properties content vs its
    /// scroll viewport — with the measured widget's objectName + geometry — so a
    /// wrong measurement target or an overflow is visible in the console.
    void logPropertiesFit();

    /// Provenance tab: show the recipe (params) of the selected product.
    void showProvenance(const QString &title, const QVariantMap &params);
    void clearProvenance();

    /// Lazily add a "Copilot" tab hosting the scientific-copilot chat panel
    /// (R-22) and return it, so the host can wire its BackendClient + dataset id.
    /// Idempotent: repeated calls return the same panel.
    AssistantPanel *ensureCopilotTab();

private:
    QVBoxLayout *groupLayout(const QString &caption);   ///< find/create an analysis group box

    QLabel *m_cloudChip = nullptr;
    QTabWidget *m_tabs = nullptr;

    QWidget *m_propsPage = nullptr;
    QScrollArea *m_propsScroll = nullptr;
    QVBoxLayout *m_propsLayout = nullptr;

    QWidget *m_analysisPage = nullptr;
    QVBoxLayout *m_analysisLayout = nullptr;
    QHash<QString, QVBoxLayout *> m_groupLayouts;

    QWidget *m_provPage = nullptr;
    QVBoxLayout *m_provLayout = nullptr;
    QString m_activeToolName;
    QLabel *m_toolCaption{ nullptr };
    QWidget *m_toolSettingsHost{ nullptr };
    QLabel *m_provCaption{ nullptr };
    QLabel *m_provTitle = nullptr;
    QLabel *m_provBody = nullptr;
    QVariantMap m_provParams;

    AssistantPanel *m_copilot = nullptr;   ///< lazily created "Copilot" tab (R-22)
};

#endif // INSPECTORPANEL_H
