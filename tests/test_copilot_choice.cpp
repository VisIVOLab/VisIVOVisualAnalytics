#include "TestCopilotChoice.h"

#include "Settings.h"
#include "app/CopilotChoice.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using CopilotChoice::Choice;

void TestCopilotChoice::serverChoice_sendsOnlyAReset()
{
    // The backend keeps what its operator configured: nothing else is sent,
    // whatever is stored next to the choice.
    const QJsonObject body = CopilotChoice::configBody(
            Choice::Server, QStringLiteral("k"), QStringLiteral("http://gpu:8000/v1"),
            QStringLiteral("m"));
    QCOMPARE(body, (QJsonObject{ { QStringLiteral("reset"), true } }));
}

void TestCopilotChoice::explicitChoices_sendEveryField()
{
    const QString key = QStringLiteral("KEY"), url = QStringLiteral("http://gpu:8000/v1"),
                  model = QStringLiteral("qwen");
    auto field = [](const QJsonObject &o, const char *name) {
        return o.value(QLatin1String(name)).toString();
    };
    const QStringList all = { QStringLiteral("provider"), QStringLiteral("anthropic_key"),
                              QStringLiteral("openai_key"), QStringLiteral("base_url"),
                              QStringLiteral("base_url_key"), QStringLiteral("model") };

    const QJsonObject claude = CopilotChoice::configBody(Choice::Anthropic, key, url, model);
    QStringList sorted = all;
    sorted.sort();
    QCOMPARE(claude.keys(), sorted);   // QJsonObject keys come sorted
    QVERIFY(!claude.contains(QStringLiteral("reset")));
    QCOMPARE(field(claude, "provider"), QStringLiteral("anthropic"));
    QCOMPARE(field(claude, "anthropic_key"), key);
    QVERIFY(field(claude, "openai_key").isEmpty());
    QVERIFY(field(claude, "base_url").isEmpty());   // clears the environment's URL
    QCOMPARE(field(claude, "model"), model);

    const QJsonObject openai = CopilotChoice::configBody(Choice::OpenAI, key, url, model);
    QCOMPARE(field(openai, "provider"), QStringLiteral("openai"));
    QCOMPARE(field(openai, "openai_key"), key);
    QVERIFY(field(openai, "base_url").isEmpty());

    // The self-hosted key never travels as the cloud key.
    const QJsonObject own = CopilotChoice::configBody(Choice::SelfHosted, key, url, model);
    QCOMPARE(field(own, "provider"), QStringLiteral("openai"));
    QCOMPARE(field(own, "base_url"), url);
    QCOMPARE(field(own, "base_url_key"), key);
    QVERIFY(field(own, "openai_key").isEmpty());
    QCOMPARE(field(own, "model"), model);

    // Off overrides the server too, and sends nothing of the stored setup.
    const QJsonObject off = CopilotChoice::configBody(Choice::Off, key, url, model);
    QCOMPARE(off.size(), all.size());
    QCOMPARE(field(off, "provider"), QStringLiteral("null"));
    for (const QString &name : all.mid(1))
        QVERIFY2(off.value(name).toString().isEmpty(), qPrintable(name));
}

void TestCopilotChoice::keys_roundTripAndUnknownIsServer()
{
    for (Choice c : { Choice::Server, Choice::Anthropic, Choice::OpenAI, Choice::SelfHosted,
                      Choice::Off })
        QCOMPARE(CopilotChoice::fromKey(CopilotChoice::toKey(c)), c);
    QCOMPARE(CopilotChoice::fromKey(QStringLiteral("something-newer")), Choice::Server);
    QCOMPARE(CopilotChoice::fromKey(QString()), Choice::Server);
}

void TestCopilotChoice::legacy_noneBecomesServerUnlessSetUpBefore()
{
    QCOMPARE(CopilotChoice::fromLegacy(0, false), Choice::Anthropic);
    QCOMPARE(CopilotChoice::fromLegacy(1, true), Choice::OpenAI);
    QCOMPARE(CopilotChoice::fromLegacy(2, true), Choice::SelfHosted);
    // None was the older default, written at first start.
    QCOMPARE(CopilotChoice::fromLegacy(3, false), Choice::Server);
    // A key or URL kept beside it: a provider was set up, then switched off.
    QCOMPARE(CopilotChoice::fromLegacy(3, true), Choice::Off);
    QCOMPARE(CopilotChoice::fromLegacy(-1, true), Choice::Server);
    QCOMPARE(CopilotChoice::fromLegacy(7, false), Choice::Server);
}

namespace {
// A Settings.ini as an older release left it.
void writeOlderSettings(const QString &dir, int provider, const QString &key)
{
    QSettings ini(dir + QStringLiteral("/Settings.ini"), QSettings::IniFormat);
    ini.setValue(QStringLiteral("Copilot/provider"), provider);
    ini.setValue(QStringLiteral("Copilot/key"), key);
    ini.setValue(QStringLiteral("Copilot/url"), QString());
    ini.sync();
}
}  // namespace

void TestCopilotChoice::settings_migrateTheOlderIndex()
{
    {
        QTemporaryDir dir;
        writeOlderSettings(dir.path(), 3, QString());
        Settings s(dir.path());
        QCOMPARE(s.getCopilotChoice(), Choice::Server);
    }
    {
        QTemporaryDir dir;
        writeOlderSettings(dir.path(), 3, QStringLiteral("sk-old"));
        Settings s(dir.path());
        QCOMPARE(s.getCopilotChoice(), Choice::Off);
    }
    {
        QTemporaryDir dir;
        writeOlderSettings(dir.path(), 2, QString());
        Settings s(dir.path());
        QCOMPARE(s.getCopilotChoice(), Choice::SelfHosted);
    }
}

void TestCopilotChoice::settings_freshFileDefaultsToServer()
{
    QTemporaryDir dir;
    Settings s(dir.path());   // no Settings.ini: the defaults are written
    QCOMPARE(s.getCopilotChoice(), Choice::Server);
    s.resetDefaults();
    QCOMPARE(s.getCopilotChoice(), Choice::Server);
}

void TestCopilotChoice::settings_choiceIsStoredWithItsLegacyIndex()
{
    QTemporaryDir dir;
    {
        Settings s(dir.path());
        s.setCopilotChoice(Choice::Anthropic);
        QCOMPARE(s.getCopilotChoice(), Choice::Anthropic);
        s.setCopilotChoice(Choice::Off);
    }
    // What an older (downgraded) release reads, and the new key wins over it.
    QSettings ini(dir.path() + QStringLiteral("/Settings.ini"), QSettings::IniFormat);
    QCOMPARE(ini.value(QStringLiteral("Copilot/provider")).toInt(), 3);
    QCOMPARE(ini.value(QStringLiteral("Copilot/choice")).toString(), QStringLiteral("off"));
    Settings again(dir.path());
    QCOMPARE(again.getCopilotChoice(), Choice::Off);
}

void TestCopilotChoice::activeLine_saysWhichConfiguration()
{
    QCOMPARE(CopilotChoice::activeLine(true, QStringLiteral("self-hosted"),
                                       QStringLiteral("qwen"), true, false),
             QStringLiteral("Provider: server's configuration (self-hosted, model qwen) · ready"));
    QCOMPARE(CopilotChoice::activeLine(true, QStringLiteral("self-hosted"), QString(), true, true),
             QStringLiteral("Provider: server's configuration (self-hosted, no model set)"
                            " · cannot answer"));
    QCOMPARE(CopilotChoice::activeLine(true, QStringLiteral("null"), QString(), false, true),
             QStringLiteral("Provider: server's configuration (off) · cannot answer"));
    QCOMPARE(CopilotChoice::activeLine(false, QStringLiteral("anthropic"), QString(), true, false),
             QStringLiteral("Provider: anthropic · ready"));
    QCOMPARE(CopilotChoice::activeLine(false, QStringLiteral("null"), QString(), false, false),
             QStringLiteral("Provider: off"));
}
