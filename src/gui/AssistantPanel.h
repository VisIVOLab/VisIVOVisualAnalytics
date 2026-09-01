#ifndef AssistantPanel_h
#define AssistantPanel_h

#include <QJsonObject>
#include <QList>
#include <QWidget>

class QLineEdit;
class QPushButton;
class QTextBrowser;
class QLabel;

/// Scientific copilot chat panel (R-22).
///
/// A thin front-end over BackendClient::assistantQuery. The copilot is a
/// provenance-bound, tool-calling orchestrator on the backend: every number in
/// an answer comes from a recorded tool call, and each answer carries a
/// reproducibility script. This panel just renders the conversation and the
/// per-answer provenance footer; it holds no scientific logic of its own.
///
/// When no LLM provider is configured server-side the backend still answers
/// (explaining how to enable one), so the panel degrades gracefully.
class AssistantPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AssistantPanel(QWidget *parent = nullptr);

    /// Backend endpoint used for queries. A synchronous BackendClient is built
    /// per query inside a worker thread (matching the rest of the app), so no
    /// long-lived client pointer is held.
    void setBackend(const QString &url, const QString &token);
    /// The dataset the questions are about (may be empty).
    void setDatasetId(const QString &datasetId);

signals:
    /// Emitted when a query starts / finishes so the host can show busy state.
    void busyChanged(bool busy);

    /// The copilot asked to change the visualisation. The host viewer applies the
    /// directives (each a {"type":…, …} object) in order; they are reversible.
    void actionsRequested(const QList<QJsonObject> &actions);

    /// The user clicked "Undo" on an applied-actions log line; the host viewer
    /// reverts the last copilot visualisation change.
    void undoRequested();

private slots:
    void onSend();
    void onSettings();   ///< choose the LLM provider (Claude / OpenAI / own server)

private:
    void appendUser(const QString &text);
    void appendAssistant(const QString &answer, const QString &provider, bool configured,
                         bool grounded, const QString &repro, const QString &provenanceHtml);
    void appendError(const QString &text);
    void setBusy(bool busy);

    QString m_url;
    QString m_token;
    QString m_datasetId;
    bool m_busy{ false };

    QTextBrowser *m_transcript{ nullptr };
    QLineEdit *m_input{ nullptr };
    QPushButton *m_send{ nullptr };
    QPushButton *m_settings{ nullptr };
    QLabel *m_status{ nullptr };
};

#endif  // AssistantPanel_h
