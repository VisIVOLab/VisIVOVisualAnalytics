#ifndef CopilotChoice_h
#define CopilotChoice_h

#include <QJsonObject>
#include <QString>

/// Which language model the desktop asks the backend's copilot to use, as
/// Settings ▸ Scientific Copilot stores it, and the /v1/assistant/config body
/// that choice sends. Widget-free so it can be unit-tested.
///
/// "Use the server's configuration" is the default: the backend then keeps
/// what its operator configured through environment variables, problems
/// included. Only an explicit choice overrides it.
namespace CopilotChoice {

enum class Choice { Server, Anthropic, OpenAI, SelfHosted, Off };

/// The stored spelling, stable across releases.
inline QString toKey(Choice c)
{
    switch (c) {
    case Choice::Anthropic:  return QStringLiteral("anthropic");
    case Choice::OpenAI:     return QStringLiteral("openai");
    case Choice::SelfHosted: return QStringLiteral("self-hosted");
    case Choice::Off:        return QStringLiteral("off");
    case Choice::Server:     break;
    }
    return QStringLiteral("server");
}

/// An unknown spelling (a newer release's choice) falls back to the server's
/// configuration, which overrides nothing.
inline Choice fromKey(const QString &key)
{
    if (key == u"anthropic") return Choice::Anthropic;
    if (key == u"openai") return Choice::OpenAI;
    if (key == u"self-hosted") return Choice::SelfHosted;
    if (key == u"off") return Choice::Off;
    return Choice::Server;
}

/// The choice behind an older release's "Copilot/provider" index (0 Claude,
/// 1 ChatGPT, 2 your server, 3 None).
///
/// None was also the default that release wrote on first start, so a stored
/// None mostly means "never chosen" and becomes the server's configuration.
/// It is kept as Off only when a key or a server URL is stored next to it: the
/// user then set a provider up once and switched it off afterwards.
inline Choice fromLegacy(int index, bool keyOrUrlStored)
{
    switch (index) {
    case 0: return Choice::Anthropic;
    case 1: return Choice::OpenAI;
    case 2: return Choice::SelfHosted;
    case 3: return keyOrUrlStored ? Choice::Off : Choice::Server;
    default: return Choice::Server;
    }
}

/// The older index to keep writing, so a downgraded desktop still reads a
/// sensible provider. It has no "server" entry: None (3) was its default.
inline int toLegacy(Choice c)
{
    switch (c) {
    case Choice::Anthropic:  return 0;
    case Choice::OpenAI:     return 1;
    case Choice::SelfHosted: return 2;
    case Choice::Off:
    case Choice::Server:     break;
    }
    return 3;
}

/// The POST /v1/assistant/config body for a choice.
///
/// Server: only ``reset``, so the session goes back to the environment's
/// configuration (a backend older than the flag ignores it and changes
/// nothing). An explicit choice sends every field, the unused ones empty, so
/// nothing of an earlier choice or of the environment is left behind.
inline QJsonObject configBody(Choice c, const QString &key, const QString &url,
                              const QString &model)
{
    if (c == Choice::Server)
        return QJsonObject{ { QStringLiteral("reset"), true } };
    QString provider = QStringLiteral("null");
    QString anthropicKey, openaiKey, baseUrl, baseUrlKey, modelName;
    switch (c) {
    case Choice::Anthropic:
        provider = QStringLiteral("anthropic");
        anthropicKey = key;
        modelName = model;
        break;
    case Choice::OpenAI:
        provider = QStringLiteral("openai");
        openaiKey = key;
        modelName = model;
        break;
    case Choice::SelfHosted:
        provider = QStringLiteral("openai");
        baseUrl = url;
        baseUrlKey = key;
        modelName = model;
        break;
    case Choice::Off:
    case Choice::Server:
        break;
    }
    return QJsonObject{ { QStringLiteral("provider"), provider },
                        { QStringLiteral("anthropic_key"), anthropicKey },
                        { QStringLiteral("openai_key"), openaiKey },
                        { QStringLiteral("base_url"), baseUrl },
                        { QStringLiteral("base_url_key"), baseUrlKey },
                        { QStringLiteral("model"), modelName } };
}

/// The provider the backend reports as active, e.g. "server's configuration
/// (self-hosted, model qwen)". @p provider is the backend's name for it (null,
/// anthropic, openai, self-hosted) and @p model the configured model ("" when
/// none is set). activeLine() makes the panel's status line of it, e.g.
/// "Provider: server's configuration (self-hosted, model qwen) · ready".
inline QString activeName(bool fromServer, const QString &provider, const QString &model)
{
    QString what = provider.isEmpty() || provider == u"null" ? QStringLiteral("off")
                                                             : provider;
    if (what != u"off") {
        if (!model.isEmpty())
            what += QStringLiteral(", model %1").arg(model);
        else if (provider == u"self-hosted")
            what += QStringLiteral(", no model set");
    }
    return fromServer ? QStringLiteral("server's configuration (%1)").arg(what) : what;
}

inline QString activeLine(bool fromServer, const QString &provider, const QString &model,
                          bool configured, bool hasProblem)
{
    QString line = QStringLiteral("Provider: %1").arg(activeName(fromServer, provider, model));
    if (hasProblem)
        line += QStringLiteral(" · cannot answer");
    else if (configured)
        line += QStringLiteral(" · ready");
    return line;
}

}  // namespace CopilotChoice

#endif  // CopilotChoice_h
