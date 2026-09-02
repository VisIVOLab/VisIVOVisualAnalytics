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
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::StringLiterals;

namespace {
QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

/// Chat-bubble colour palette, chosen per light/dark theme. QTextDocument does
/// not resolve `palette(...)`, so we pick concrete colours from the widget's
/// actual palette lightness.
struct BubbleTheme
{
    QString userBg, userFg, botBg, botFg, meta, rule, errBg, errFg;
};
BubbleTheme bubbleTheme(const QPalette &pal)
{
    const bool dark = pal.color(QPalette::Base).lightness() < 128;
    if (dark) {
        return { u"#2b4a6f"_s, u"#eaf1fb"_s, u"#2a2d31"_s, u"#e6e6e6"_s,
                 u"#9aa0a6"_s, u"#3a3d42"_s, u"#4a2626"_s, u"#f3b9b9"_s };
    }
    return { u"#dbe8fb"_s, u"#173a63"_s, u"#f1f2f4"_s, u"#1b1c1d"_s,
             u"#6b7075"_s, u"#e2e4e8"_s, u"#fbe3e3"_s, u"#8a1f1f"_s };
}
}  // namespace

AssistantPanel::AssistantPanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto *titleRow = new QHBoxLayout();
    auto *title = new QLabel(u"Scientific copilot"_s, this);
    title->setStyleSheet(u"font-weight: 600;"_s);
    m_clear = new QPushButton(u"＋ New chat"_s, this);
    m_clear->setToolTip(u"Start a fresh conversation (clears the follow-up context)"_s);
    m_clear->setFlat(true);
    m_clear->setCursor(Qt::PointingHandCursor);
    m_clear->setStyleSheet(u"QPushButton { color: palette(mid); border: none; padding: 2px 6px; }"
                           "QPushButton:hover { color: palette(text); }"_s);
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    titleRow->addWidget(m_clear);
    root->addLayout(titleRow);

    m_transcript = new QTextBrowser(this);
    m_transcript->setOpenExternalLinks(false);
    m_transcript->setOpenLinks(false);   // handle the "Undo" anchor ourselves
    m_transcript->setFrameShape(QFrame::NoFrame);
    connect(m_transcript, &QTextBrowser::anchorClicked, this, [this](const QUrl &u) {
        if (u.toString() == u"copilot:undo"_s)
            emit undoRequested();
    });
    m_transcript->setPlaceholderText(
            u"Ask about the loaded dataset — units, statistics, a moment map, a "
            "distance. Every number in the answer is backed by a tool call.\n\n"
            "Ask follow-up questions freely — the copilot remembers the conversation."_s);
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
    m_input->setClearButtonEnabled(true);
    m_send = new QPushButton(u"Send"_s, this);
    m_send->setDefault(true);
    row->addWidget(m_settings);
    row->addWidget(m_input, 1);
    row->addWidget(m_send);
    root->addLayout(row);

    connect(m_settings, &QPushButton::clicked, this, &AssistantPanel::onSettings);
    connect(m_send, &QPushButton::clicked, this, &AssistantPanel::onSend);
    connect(m_input, &QLineEdit::returnPressed, this, &AssistantPanel::onSend);
    connect(m_clear, &QPushButton::clicked, this, &AssistantPanel::onClearConversation);
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
    if (endpointChanged) {
        m_providerAutoApplied = false;   // a new endpoint must get the saved provider
        // A new backend = a new session/datasets — the follow-up context no longer
        // applies. (Harmless on the first connect: nothing to clear.)
        if (!m_turns.isEmpty() || !m_entries.isEmpty()) {
            m_turns = QJsonArray{};
            m_entries.clear();
            if (m_transcript)
                renderConversation();
        }
    }
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
    if (datasetId == m_datasetId)
        return;
    // The conversation is ABOUT a specific dataset — when it changes (e.g. a
    // session-expiry reopen gives a new id), drop the follow-up context and
    // transcript so old Q&A can't be replayed against a different dataset.
    m_datasetId = datasetId;
    m_turns = QJsonArray{};
    m_entries.clear();
    renderConversation();
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

void AssistantPanel::appendError(const QString &text)
{
    Entry e;
    e.kind = Entry::Error;
    e.text = text;
    m_entries.push_back(e);
    renderConversation();
}

void AssistantPanel::onClearConversation()
{
    if (m_busy)
        return;
    m_entries.clear();
    m_turns = QJsonArray{};
    m_input->clear();
    renderConversation();
}

void AssistantPanel::renderConversation()
{
    const BubbleTheme t = bubbleTheme(this->palette());

    // A right- or left-aligned chat bubble, laid out with a 2-cell outer table so
    // alignment renders reliably in QTextDocument (no flexbox / border-radius).
    auto bubble = [](bool rightAlign, const QString &bg, const QString &inner) {
        const QString gap = u"<td width='16%'></td>"_s;
        const QString cell =
                u"<td width='84%'><table width='100%' cellpadding='8' cellspacing='0' "
                "bgcolor='%1'><tr><td>%2</td></tr></table></td>"_s.arg(bg, inner);
        return u"<table width='100%' cellspacing='0' cellpadding='0' "
               "style='margin:5px 0;'><tr>%1</tr></table>"_s
                .arg(rightAlign ? (gap + cell) : (cell + gap));
    };

    QString html = u"<div style='font-family:sans-serif;'>"_s;
    for (const Entry &e : m_entries) {
        if (e.kind == Entry::User) {
            const QString inner =
                    u"<span style='color:%1;font-size:small;'>You</span><br>"
                    "<span style='color:%2;'>%3</span>"_s
                            .arg(t.meta, t.userFg, esc(e.text).replace(u"\n"_s, u"<br>"_s));
            html += bubble(/*rightAlign=*/true, t.userBg, inner);
        } else if (e.kind == Entry::Assistant) {
            QString inner = u"<span style='color:%1;font-size:small;'>Copilot"_s.arg(t.meta);
            if (!e.provider.isEmpty())
                inner += u" · %1"_s.arg(esc(e.provider));
            inner += u"</span><br><span style='color:%1;'>%2</span>"_s
                             .arg(t.botFg, esc(e.text).replace(u"\n"_s, u"<br>"_s));
            if (!e.configured) {
                inner += u"<br><span style='color:%1;font-size:small;'><i>No LLM provider is "
                         "configured on the server — the analysis tools still work; open ⚙ to "
                         "enable natural-language answers.</i></span>"_s.arg(t.meta);
            }
            if (!e.provenance.isEmpty()) {
                inner += u"<div style='margin-top:6px;border-top:1px solid %1;padding-top:4px;'>"
                         "<span style='color:%2;font-size:small;'>%3 · grounded in tool call(s):"
                         "</span><div style='color:%2;font-size:small;'>%4</div></div>"_s
                                 .arg(t.rule, t.meta,
                                      e.grounded ? u"✓ provenance"_s : u"provenance"_s,
                                      e.provenance);
            }
            if (!e.repro.isEmpty()) {
                inner += u"<div style='margin-top:4px;'><span style='color:%1;font-size:small;'>"
                         "Reproducibility</span><pre style='color:%1;font-size:small;margin:2px 0;'>"
                         "%2</pre></div>"_s.arg(t.meta, esc(e.repro));
            }
            html += bubble(/*rightAlign=*/false, t.botBg, inner);
        } else if (e.kind == Entry::ActionLog) {
            html += u"<table width='100%' cellspacing='0' cellpadding='6' style='margin:2px 0;'>"
                    "<tr><td><span style='color:%1;font-size:small;'>▸ Applied to the view "
                    "[<a href='copilot:undo' style='color:%1;'>↩ Undo latest change</a>]</span>"
                    "<span style='color:%1;font-size:small;'>%2</span></td></tr></table>"_s
                            .arg(t.meta, e.actionLog);
        } else {   // Error
            html += bubble(/*rightAlign=*/false, t.errBg,
                           u"<span style='color:%1;'><b>Error:</b> %2</span>"_s
                                   .arg(t.errFg, esc(e.text).replace(u"\n"_s, u"<br>"_s)));
        }
    }
    html += u"</div>"_s;

    m_transcript->setHtml(html);
    // Keep the newest message in view.
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
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
    { Entry e; e.kind = Entry::User; e.text = question; m_entries.push_back(e); }
    renderConversation();
    m_input->clear();
    setBusy(true);

    const QString datasetId = m_datasetId;
    const QString url = m_url;
    const QString token = m_token;
    const QJsonArray history = m_turns;   // prior turns → follow-up context
    auto *watcher = new QFutureWatcher<BackendAssistantResult>(this);
    connect(watcher, &QFutureWatcher<BackendAssistantResult>::finished, this,
            [this, watcher, question, datasetId, url, token]() {
                const BackendAssistantResult r = watcher->result();
                watcher->deleteLater();
                setBusy(false);
                // Discard a result whose conversation scope changed while it ran
                // (the dataset OR the backend endpoint was swapped) — it must not
                // land in the new conversation or its follow-up context.
                if (m_datasetId != datasetId || m_url != url || m_token != token)
                    return;
                if (!r.error.isEmpty()) {
                    appendError(r.error);
                    return;
                }
                QString prov;
                for (const BackendAssistantToolCall &c : r.toolCalls) {
                    const BubbleTheme t = bubbleTheme(this->palette());
                    prov += u"<div><code>%1(%2)</code>%3</div>"_s.arg(
                            esc(c.name), esc(c.argumentsJson),
                            c.error.isEmpty()
                                    ? u" → <span style='color:%1'>%2</span>"_s.arg(
                                              t.meta, esc(c.resultJson))
                                    : u" → <span style='color:%1'>%2</span>"_s.arg(
                                              t.errFg, esc(c.error)));
                }
                Entry a;
                a.kind = Entry::Assistant;
                a.text = r.answer;
                a.provider = r.provider;
                a.configured = r.configured;
                a.grounded = r.toolCallsRecorded;
                a.repro = r.reproducibilityScript;
                a.provenance = prov;
                m_entries.push_back(a);
                if (!r.actions.isEmpty()) {
                    QString log;
                    for (const QJsonObject &act : r.actions) {
                        QString desc = act.value(u"type"_s).toString();
                        for (auto it = act.constBegin(); it != act.constEnd(); ++it) {
                            if (it.key() == u"type"_s)
                                continue;
                            // Skip bulky payloads (e.g. a pinned spectrum's samples).
                            if (it.value().isArray() || it.value().isObject())
                                continue;
                            desc += u" %1=%2"_s.arg(it.key(), it.value().toVariant().toString());
                        }
                        log += u" · %1"_s.arg(esc(desc));
                    }
                    Entry le; le.kind = Entry::ActionLog; le.actionLog = log;
                    m_entries.push_back(le);
                    emit actionsRequested(r.actions);
                }
                // Record this exchange as follow-up context for the next turn.
                // Clamp each turn's text to the server's per-turn cap (4000) so a
                // long answer can't make the next follow-up fail validation, and
                // cap the retained turn count so requests stay bounded.
                m_turns.push_back(QJsonObject{ { u"role"_s, u"user"_s },
                                               { u"text"_s, question.left(4000) } });
                m_turns.push_back(QJsonObject{ { u"role"_s, u"assistant"_s },
                                               { u"text"_s, r.answer.left(4000) } });
                while (m_turns.size() > 20)
                    m_turns.removeFirst();
                renderConversation();
            });
    watcher->setFuture(QtConcurrent::run([url, token, question, datasetId, history]() {
        BackendClient client(url, token);
        return client.assistantQuery(question, datasetId, history);
    }));
}
