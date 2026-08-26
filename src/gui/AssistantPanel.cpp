#include "AssistantPanel.h"

#include "app/BackendClient.h"

#include <QFuture>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::StringLiterals;

namespace {
QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}
}  // namespace

AssistantPanel::AssistantPanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto *title = new QLabel(u"Scientific copilot"_s, this);
    title->setStyleSheet(u"font-weight: 600;"_s);
    root->addWidget(title);

    m_transcript = new QTextBrowser(this);
    m_transcript->setOpenExternalLinks(false);
    m_transcript->setPlaceholderText(
            u"Ask about the loaded dataset — units, statistics, a moment map, a "
            "distance. Every number in the answer is backed by a tool call."_s);
    root->addWidget(m_transcript, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(u"color: palette(mid);"_s);
    root->addWidget(m_status);

    auto *row = new QHBoxLayout();
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(u"Ask a question…"_s);
    m_send = new QPushButton(u"Send"_s, this);
    row->addWidget(m_input, 1);
    row->addWidget(m_send);
    root->addLayout(row);

    connect(m_send, &QPushButton::clicked, this, &AssistantPanel::onSend);
    connect(m_input, &QLineEdit::returnPressed, this, &AssistantPanel::onSend);
}

void AssistantPanel::setBackend(const QString &url, const QString &token)
{
    m_url = url;
    m_token = token;
}

void AssistantPanel::setDatasetId(const QString &datasetId)
{
    m_datasetId = datasetId;
}

void AssistantPanel::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    m_send->setEnabled(!busy);
    m_input->setEnabled(!busy);
    m_status->setText(busy ? u"Thinking…"_s : QString());
    emit busyChanged(busy);
}

void AssistantPanel::appendUser(const QString &text)
{
    m_transcript->append(u"<p><b>You:</b> %1</p>"_s.arg(esc(text)));
}

void AssistantPanel::appendError(const QString &text)
{
    m_transcript->append(u"<p style='color:#c0392b'><b>Error:</b> %1</p>"_s.arg(esc(text)));
}

void AssistantPanel::appendAssistant(const QString &answer, const QString &provider,
                                     bool configured, bool grounded, const QString &repro,
                                     const QString &provenanceHtml)
{
    QString html = u"<p><b>Copilot:</b> %1</p>"_s.arg(esc(answer).replace(u"\n"_s, u"<br>"_s));
    if (!configured) {
        html += u"<p style='color:palette(mid)'><i>No LLM provider is configured on "
                "the server — set ANTHROPIC_API_KEY to enable natural-language "
                "answers. The analysis tools still work.</i></p>"_s;
    }
    if (!provenanceHtml.isEmpty()) {
        html += u"<details><summary style='color:palette(mid)'>Provenance (%1) · "
                "%2 · %3 tool call(s)</summary>%4</details>"_s
                        .arg(provider, configured ? u"configured"_s : u"not configured"_s,
                             grounded ? u"with"_s : u"no"_s, provenanceHtml);
    }
    if (!repro.isEmpty()) {
        html += u"<details><summary style='color:palette(mid)'>Reproducibility "
                "script</summary><pre>%1</pre></details>"_s.arg(esc(repro));
    }
    m_transcript->append(html);
}

void AssistantPanel::onSend()
{
    if (m_busy)
        return;
    const QString question = m_input->text().trimmed();
    if (question.isEmpty())
        return;
    if (m_url.isEmpty()) {
        appendError(u"No backend connection."_s);
        return;
    }
    appendUser(question);
    m_input->clear();
    setBusy(true);

    const QString datasetId = m_datasetId;
    const QString url = m_url;
    const QString token = m_token;
    auto *watcher = new QFutureWatcher<BackendAssistantResult>(this);
    connect(watcher, &QFutureWatcher<BackendAssistantResult>::finished, this,
            [this, watcher]() {
                const BackendAssistantResult r = watcher->result();
                watcher->deleteLater();
                setBusy(false);
                if (!r.error.isEmpty()) {
                    appendError(r.error);
                    return;
                }
                QString prov;
                for (const BackendAssistantToolCall &c : r.toolCalls) {
                    prov += u"<div><code>%1(%2)</code>%3</div>"_s.arg(
                            esc(c.name), esc(c.argumentsJson),
                            c.error.isEmpty()
                                    ? u" → <span style='color:palette(mid)'>%1</span>"_s.arg(
                                              esc(c.resultJson))
                                    : u" → <span style='color:#c0392b'>%1</span>"_s.arg(
                                              esc(c.error)));
                }
                appendAssistant(r.answer, r.provider, r.configured, r.toolCallsRecorded,
                                r.reproducibilityScript, prov);
            });
    watcher->setFuture(QtConcurrent::run([url, token, question, datasetId]() {
        BackendClient client(url, token);
        return client.assistantQuery(question, datasetId);
    }));
}
