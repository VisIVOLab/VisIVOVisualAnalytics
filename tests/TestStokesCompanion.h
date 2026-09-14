#pragma once
// TestStokesCompanion.h
// QTest class for StokesCompanion - the naming rules that find Q/U/V next to a
// Stokes I image. Header-only pure logic; no GUI link.

#include <QObject>

class TestStokesCompanion : public QObject
{
    Q_OBJECT

private slots:
    void stokesInName_isReplacedInEachCasing();
    void askapSuffix_isReplacedIncludingTheGzipVariant();
    void theStokesIFileItself_isNeverACandidate();
    void pick_prefersTheStokesNamePatternOverTheBareSuffix();
    void pick_takesTheFirstCandidateThatExists();
    void pick_returnsNothingWhenTheCompanionIsAbsent();
    void aNameThatMatchesNoPattern_yieldsNoCandidates();
};
