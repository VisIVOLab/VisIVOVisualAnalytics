#pragma once
// TestDatasetName.h
// QTest class for how a dataset's path is shown and named, a field of a VBT
// volume included (issue #83). Header-only logic; no window.

#include <QObject>

class TestDatasetName : public QObject
{
    Q_OBJECT

private slots:
    void aFileKeepsItsName();
    void aFieldIsShownAsVolumeAndField();
    void aFieldNamesItsExportsAfterVolumeAndField();
};
