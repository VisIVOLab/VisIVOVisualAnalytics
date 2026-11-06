#pragma once
// TestCopilotStatus.h
// QTest class for CopilotStatus: what the copilot panels heard about why the
// copilot cannot answer, shared with Settings ▸ Scientific Copilot. Core-only.

#include <QObject>

class TestCopilotStatus : public QObject
{
    Q_OBJECT

private slots:
    void report_isReadBackPerBackend();
    void report_emptyClearsTheProblem();
    void problems_listsOnlyBackendsALivePanelUses();
    void destroyedReporter_doesNotHearItsOwnDeparture();
    void changed_onlyWhenSomethingChanged();
    void lateReplyFromAnotherPanel_doesNotUndoANewerOne();
};
