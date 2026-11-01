#include "LaunchOptions.h"

#include <QDir>
#include <QFile>
#include <QUrl>

#include <utility>

using namespace Qt::StringLiterals;

namespace {

/// The value of @p option in @p args (`--option value` or `--option=value`):
/// nullopt when the option is absent, an empty string when it is given without
/// a value (or followed by another option).
std::optional<QString> optionValue(const QStringList &args, const QString &option)
{
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == option) {
            if (i + 1 < args.size() && !args.at(i + 1).startsWith(u"--"_s))
                return args.at(i + 1).trimmed();
            return QString(u""_s);
        }
        if (a.startsWith(option + u'='))
            return a.mid(option.size() + 1).trimmed();
    }
    return std::nullopt;
}

}  // namespace

const QStringList &LaunchOptions::backendValueOptions()
{
    static const QStringList options = { u"--backend"_s, u"--backend-token-file"_s,
                                         u"--backend-name"_s };
    return options;
}

bool LaunchOptions::remoteBackend(const QStringList &args, const QProcessEnvironment &env,
                                  std::optional<Settings::SessionBackend> *out, QString *error)
{
    const auto fromEnv = [&env](const char *name) {
        return env.value(QString::fromLatin1(name)).trimmed();
    };
    // An option given without its value is a mistake to report, not a request
    // for the local backend.
    const auto urlOpt = optionValue(args, u"--backend"_s);
    const auto tokenFileOpt = optionValue(args, u"--backend-token-file"_s);
    const auto nameOpt = optionValue(args, u"--backend-name"_s);
    for (const auto &[opt, what] :
         { std::pair{ &urlOpt, u"--backend needs a URL (http://host:port)."_s },
           std::pair{ &tokenFileOpt, u"--backend-token-file needs a file."_s },
           std::pair{ &nameOpt, u"--backend-name needs a name."_s } }) {
        if (*opt && (*opt)->isEmpty()) {
            *error = what;
            return false;
        }
    }
    QString url = urlOpt ? *urlOpt : fromEnv("VISIVO_BACKEND_URL");
    if (url.isEmpty()) {
        if (tokenFileOpt || nameOpt) {
            *error = u"--backend-token-file and --backend-name go with --backend URL."_s;
            return false;
        }
        return true;   // the local backend
    }

    while (url.endsWith(u'/'))
        url.chop(1);
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty()
        || (parsed.scheme() != u"http"_s && parsed.scheme() != u"https"_s)) {
        *error = u"\"%1\" is not a backend address: it needs the form http://host:port."_s.arg(url);
        return false;
    }

    Settings::SessionBackend backend;
    backend.url = url;
    QString tokenFile = tokenFileOpt ? *tokenFileOpt : fromEnv("VISIVO_BACKEND_TOKEN_FILE");
    if (!tokenFile.isEmpty()) {
        if (tokenFile.startsWith(u"~/"_s))
            tokenFile = QDir::home().absoluteFilePath(tokenFile.mid(2));
        QFile f(tokenFile);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            *error = u"Cannot read the backend token from %1: %2"_s.arg(tokenFile, f.errorString());
            return false;
        }
        backend.token = QString::fromUtf8(f.readAll()).trimmed();
        if (backend.token.isEmpty()) {
            *error = u"The backend token file %1 is empty."_s.arg(tokenFile);
            return false;
        }
    } else {
        backend.token = fromEnv("VISIVO_BACKEND_TOKEN");
    }

    backend.name = nameOpt ? *nameOpt : fromEnv("VISIVO_BACKEND_NAME");
    if (backend.name.isEmpty())
        backend.name = parsed.port() > 0 ? u"%1:%2"_s.arg(parsed.host()).arg(parsed.port())
                                         : parsed.host();
    *out = backend;
    return true;
}
