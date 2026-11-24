#pragma once
// TestImageLayerContext.h
// QTest class for what the image viewer shows about the selected layer
// (ImageLayerContext.h): which file's header, which BUNIT, how an axis reads.

#include <QObject>

class TestImageLayerContext : public QObject
{
    Q_OBJECT

private slots:
    void theSelectedRowGivesItsOwnFile();
    void noSelectionFallsBackToTheBaseFile();
    void bunitIsReadFromTheCardsOfThatLayer();
    void aSlashInTheUnitIsKept();
    void commentaryCardsHaveNoValue();
    void aCollapsedAxisIsNamedAsAnAxis();
    void aSkyAxisKeepsItsUnitOnly();
};
