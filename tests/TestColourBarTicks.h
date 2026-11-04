#pragma once
// TestColourBarTicks.h
// QTest class for the labels down a colour bar (ColourBarTicks.h): one
// notation for every tick. Header-only logic; no window.

#include <QObject>

class TestColourBarTicks : public QObject
{
    Q_OBJECT

private slots:
    void smallValuesKeepThreeDigits();
    void aVelocityIsWrittenOut();
    void farApartValuesTakeExponents();
};
