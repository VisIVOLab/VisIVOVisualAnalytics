#include "AssistantPanel.h"

#include "Settings.h"
#include "app/BackendClient.h"
#include "app/BackendClientFactory.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
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
    m_transcript->setOpenLinks(false);   // handle the "Undo" anchor ourselves
    connect(m_transcript, &QTextBrowser::anchorClicked, this, [this](const QUrl &u) {
        if (u.toString() == u"copilot:undo"_s)
            emit undoRequested();
    });
    m_transcript->setPlaceholderText(
            u"Ask about the loaded dataset — units, statistics, a moment map, a "
            "distance. Every number in the answer is backed by a tool call."_s);
    root->addWidget(m_transcript, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(u"color: palette(mid);"_s);
    root->addWidget(m_status);

    auto *row = new QHBoxLayout();
    m_settings = new QPushButton(u"⚙"_s, this);
    m_settings->setToolTip(u"Choose the AI provider (Claude / OpenAI / your server)"_s);
    m_settings->setFixedWidth(32);
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(u"Ask a question…"_s);
    m_send = new QPushButton(u"Send"_s, this);
    row->addWidget(m_settings);
    row->addWidget(m_input, 1);
    row->addWidget(m_send);
    root->addLayout(row);

    connect(m_settings, &QPushButton::clicked, this, &AssistantPanel::onSettings);
    connect(m_send, &QPushButton::clicked, this, &AssistantPanel::onSend);
    connect(m_input, &QLineEdit::returnPressed, this, &AssistantPanel::onSend);
}

void AssistantPanel::onSettings()
{
    if (m_url.isEmpty()) {
        appendError(u"No backend connection."_s);
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(u"AI provider"_s);
    auto *form = new QFormLayout(&dlg);

    auto *provider = new QComboBox(&dlg);
    // userData = the backend provider key; index order fixed below.
    provider->addItem(u"Claude (Anthropic)"_s, u"anthropic"_s);
    provider->addItem(u"ChatGPT (OpenAI)"_s, u"openai"_s);
    provider->addItem(u"Your server (OpenAI-compatible)"_s, u"openai"_s);
    provider->addItem(u"None"_s, u"null"_s);

    auto *key = new QLineEdit(&dlg);
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(u"API key"_s);
    auto *url = new QLineEdit(&dlg);
    url->setPlaceholderText(u"http://your-server:8000/v1"_s);
    auto *model = new QLineEdit(&dlg);
    model->setPlaceholderText(u"model name (optional)"_s);

    form->addRow(u"Provider"_s, provider);
    form->addRow(u"API key"_s, key);
    form->addRow(u"Server URL"_s, url);
    form->addRow(u"Model"_s, model);

    // Show only the relevant field(s) per provider: cloud → key; own server →
    // URL (+ optional key); none → nothing.
    auto refresh = [&]() {
        const int i = provider->currentIndex();
        key->setVisible(i == 0 || i == 1 || i == 2);
        url->setVisible(i == 2);
        model->setVisible(i != 3);
        form->labelForField(key)->setVisible(key->isVisible());
        form->labelForField(url)->setVisible(url->isVisible());
        form->labelForField(model)->setVisible(model->isVisible());
    };
    connect(provider, &QComboBox::currentIndexChanged, &dlg, [&](int) { refresh(); });
    // Pre-fill from the persisted config so the user doesn't re-enter it.
    if (Settings *s = BackendClientFactory::instance()->settings()) {
        provider->setCurrentIndex(s->getCopilotProvider());
        key->setText(s->getCopilotKey());
        url->setText(s->getCopilotUrl());
        model->setText(s->getCopilotModel());
    }
    refresh();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // Send the COMPLETE state for the chosen provider; every unselected field is
    // cleared server-side so switching fully resets stale keys/URL/model. The
    // self-hosted endpoint key goes to base_url_key (NEVER openai_key) so a
    // stored cloud key can't be redirected to an arbitrary server.
    const int idx = provider->currentIndex();
    const QString prov = provider->currentData().toString();
    const QString k = key->text();
    const QString m = model->text().trimmed();
    // Persist the choice so it auto-applies on the next connect (the user asked
    // not to re-enter it every time).
    if (Settings *s = BackendClientFactory::instance()->settings()) {
        s->setCopilotProvider(idx);
        s->setCopilotKey(k);
        s->setCopilotUrl(url->text().trimmed());
        s->setCopilotModel(m);
    }
    QString anthropicKey, openaiKey, baseUrl, baseUrlKey;
    if (idx == 0)
        anthropicKey = k;                       // Claude
    else if (idx == 1)
        openaiKey = k;                          // ChatGPT (OpenAI cloud)
    else if (idx == 2) {                         // Your server (self-hosted)
        baseUrl = url->text().trimmed();
        baseUrlKey = k;
    }

    setBusy(true);
    m_status->setText(u"Applying provider…"_s);
    const QString url0 = m_url, token0 = m_token;
    auto *watcher = new QFutureWatcher<BackendAssistantConfigResult>(this);
    connect(watcher, &QFutureWatcher<BackendAssistantConfigResult>::finished, this,
            [this, watcher]() {
                const BackendAssistantConfigResult r = watcher->result();
                watcher->deleteLater();
                setBusy(false);
                if (!r.error.isEmpty()) {
                    appendError(r.error);
                    return;
                }
                m_status->setText(r.configured
                        ? u"Provider: %1 (ready)"_s.arg(r.provider)
                        : u"Provider: %1 (not configured)"_s.arg(r.provider));
            });
    watcher->setFuture(QtConcurrent::run(
            [url0, token0, prov, anthropicKey, openaiKey, baseUrl, baseUrlKey, m]() {
                BackendClient client(url0, token0);
                return client.setAssistantConfig(prov, anthropicKey, openaiKey, baseUrl,
                                                 baseUrlKey, m);
            }));
}

void AssistantPanel::setBackend(const QString &url, const QString &token)
{
    const bool endpointChanged = (url != m_url || token != m_token);
    m_url = url;
    m_token = token;
    if (endpointChanged)
        m_providerAutoApplied = false;   // a new endpoint must get the saved provider
    // Re-apply whenever the user edits the copilot provider in the global Settings
    // dialog (it persists + emits updated()). A real member slot means
    // Qt::UniqueConnection actually de-dupes across repeated setBackend() calls.
    if (Settings *s = BackendClientFactory::instance()->settings())
        connect(s, &Settings::updated, this, &AssistantPanel::onSettingsUpdated,
                Qt::UniqueConnection);
    // Auto-apply the persisted provider so the user never re-enters it.
    this->applyProviderFromSettings();
}

void AssistantPanel::onSettingsUpdated()
{
    m_providerAutoApplied = false;   // force a re-apply on a settings change
    this->applyProviderFromSettings();
}

void AssistantPanel::applyProviderFromSettings()
{
    // Auto-apply only once (first connect), so a later manual ⚙ change can't be
    // overwritten by a repeated/stale connect-time POST.
    if (m_providerAutoApplied)
        return;
    Settings *s = BackendClientFactory::instance()->settings();
    if (!s || m_url.isEmpty())
        return;
    m_providerAutoApplied = true;
    const int idx = s->getCopilotProvider();
    const QString key = s->getCopilotKey();
    const QString model = s->getCopilotModel();
    // Default "null" so a persisted None explicitly resets the backend provider.
    QString prov = u"null"_s, anthropicKey, openaiKey, baseUrl, baseUrlKey;
    if (idx == 0) {
        prov = u"anthropic"_s;
        anthropicKey = key;
    } else if (idx == 1) {
        prov = u"openai"_s;
        openaiKey = key;
    } else if (idx == 2) {
        prov = u"openai"_s;
        baseUrl = s->getCopilotUrl();
        baseUrlKey = key;
    }
    // Fire-and-forget: configure the backend in the background; the panel's next
    // query/status reflects it. Failures are non-fatal (the user can re-open ⚙).
    const QString url0 = m_url, token0 = m_token;
    (void)QtConcurrent::run(
            [url0, token0, prov, anthropicKey, openaiKey, baseUrl, baseUrlKey, model]() {
                BackendClient client(url0, token0);
                client.setAssistantConfig(prov, anthropicKey, openaiKey, baseUrl, baseUrlKey, model);
            });
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
                if (!r.actions.isEmpty()) {
                    // Log what the copilot is about to change, then hand the
                    // directives to the host viewer to apply (reversible).
                    QString log;
                    for (const QJsonObject &a : r.actions) {
                        QString desc = a.value(u"type"_s).toString();
                        for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
                            if (it.key() == u"type"_s)
                                continue;
                            desc += u" %1=%2"_s.arg(it.key(), it.value().toVariant().toString());
                        }
                        log += u"<div>▸ %1</div>"_s.arg(esc(desc));
                    }
                    m_transcript->append(
                            u"<p style='color:palette(mid)'><i>Applied to the view</i> "
                            "[<a href='copilot:undo'>↩ Undo latest change</a>]:%1</p>"_s.arg(log));
                    emit actionsRequested(r.actions);
                }
            });
    watcher->setFuture(QtConcurrent::run([url, token, question, datasetId]() {
        BackendClient client(url, token);
        return client.assistantQuery(question, datasetId);
    }));
}
