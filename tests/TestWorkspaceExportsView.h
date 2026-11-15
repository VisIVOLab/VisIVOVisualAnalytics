#pragma once
// TestWorkspaceExportsView.h
// QTest class for what the Data Hub's Workspace Exports list shows of a
// listing (app/WorkspaceExportsView.h, issue #312): the fingerprint that
// spares an unchanged list its rebuild, and the cap on the rows.

#include <QObject>

class TestWorkspaceExportsView : public QObject
{
    Q_OBJECT

private slots:
    void anUnchangedListingHasTheSameSignature();
    void anyChangeShownInARowChangesTheSignature();
    void aFailedListingDiffersFromAnEmptyOne();
    void onlyTheMostRecentExportsGetARow();
};
