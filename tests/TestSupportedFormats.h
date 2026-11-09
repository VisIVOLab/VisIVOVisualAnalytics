#pragma once
// TestSupportedFormats.h
// QTest class for the table of file types the file browser offers and the
// name filter behind its drop-down (issue #153). Header-only logic; no window.

#include <QObject>

class TestSupportedFormats : public QObject
{
    Q_OBJECT

private slots:
    void everyTypeHasARowAndAStableKey();
    void allSupportedIsTheUnionWithoutRepeats();
    void multiPartSuffixesMatchWhole();
    void matchingIgnoresCase();
    void dotfilesAreListedOnlyUnderAllFiles();
    void allFilesListsAnything();
    void otherKindsAreNotListed();
    void displayLabelShowsPatterns();
    void allFilesIsAlwaysOffered();
    void initialTypePrefersTheRememberedOne();
};
