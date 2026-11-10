#pragma once
// TestVbtByteOrder.h
// QTest class for a VBT whose header declares the wrong byte order (issue
// #165): the open answer's warning, the question's wording and default, the
// remembered choice, and the recipe that records it. Header-only logic.

#include <QObject>

class TestVbtByteOrder : public QObject
{
    Q_OBJECT

private slots:
    void theWarningIsParsedFromTheOpenAnswer();
    void noWarningOrANonsenseOneIsAbsent();
    void theQuestionNamesBothOrdersAndTheEvidence();
    void onlyOverwhelmingEvidenceDefaultsToTheSuggestedOrder();
    void theChoiceIsRememberedPerBackendAndDataFile();
    void theRecipeRecordsTheChosenOrder();
};
