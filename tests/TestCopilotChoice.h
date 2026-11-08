#pragma once
// TestCopilotChoice.h
// QTest class for CopilotChoice — the copilot provider choice in Settings, the
// /v1/assistant/config body it sends, and the migration of the older stored
// index. Core-only.

#include <QObject>

class TestCopilotChoice : public QObject
{
    Q_OBJECT

private slots:
    void serverChoice_sendsOnlyAReset();
    void explicitChoices_sendEveryField();
    void keys_roundTripAndUnknownIsServer();
    void legacy_noneBecomesServerUnlessSetUpBefore();
    void settings_migrateTheOlderIndex();
    void settings_freshFileDefaultsToServer();
    void settings_choiceIsStoredWithItsLegacyIndex();
    void activeLine_saysWhichConfiguration();
};
