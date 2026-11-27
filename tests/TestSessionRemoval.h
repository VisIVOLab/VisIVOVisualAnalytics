#pragma once
// TestSessionRemoval.h
// QTest class for SessionRemoval.h — what removing products from Session Data
// covers, asks and deletes (issue #580). Core-only; no widget.

#include <QObject>

class TestSessionRemoval : public QObject
{
    Q_OBJECT

private slots:
    void savedPermanently_onlyAWorkspaceExportCounts();
    void plan_dropsUnknownAndRepeatedIds();
    void plan_aRunningOrLiveProductStays();
    void plan_aToolThatNamesItselfRunningInItsParametersStays();
    void plan_theWindowCanRefuseAProduct();
    void plan_listsWhatWasMadeFromTheProductsThatGo();
    void plan_aDependantAskedForIsNotListedTwice();
    void plan_aDependantThatCannotGoIsLeftOut();
    void confirmation_onlyWhenSomethingWouldBeLost();
    void confirmationText_namesTheProductsAndWhatWasMadeFromThem();
    void temporaryFile_onlyATemporaryOneUnderTheTempRoot();
};
