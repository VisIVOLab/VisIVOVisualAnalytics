#pragma once
// TestRecentReopen.h
// QTest class for reopening a Recent Datasets entry whose file has gone
// (#150): which entries are checked, where the file is looked for, how the
// backend's "not found" is told apart, and removing only the entry clicked.

#include <QObject>

class TestRecentReopen : public QObject
{
    Q_OBJECT

private slots:
    void entriesThatNameAFile();
    void nodeFilesAreNotLookedForHere();
    void aFieldIsLookedForAsItsDataFile();
    void aFileBehindAClosedFolderIsNotCalledMissing();
    void onlyTheClassifiersNotFoundIsMissing();
    void theMessageSaysWhere();
    void removingAnEntryRemovesOnlyThatOne();
};
