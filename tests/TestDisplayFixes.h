#pragma once
// TestDisplayFixes.h
// QTest class for the small display fixes of issue #544 whose logic stands alone:
// a font scaled without a negative size, a header the backend sent empty, and the
// X/Y/Z axes of a simulation box.

#include <QObject>

class TestDisplayFixes : public QObject
{
    Q_OBJECT

private slots:
    void aFontSizeIsNeverScaledBelowItsMinimum();
    void anEmptyHeaderIsPresentNotMissing();
    void anAbsentRequiredHeaderIsMissing();
    void anOptionalHeaderIsNeverMissing();
    void theAxesOfASimulationBoxAreLinearSpace();
};
