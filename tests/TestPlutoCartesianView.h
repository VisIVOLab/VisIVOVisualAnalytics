#pragma once
// TestPlutoCartesianView.h
// QTest class for app/PlutoCartesianView.h: the plan the backend sends for the
// Cartesian view of a PLUTO output, how it is worded, and the request (#527).

#include <QObject>

class TestPlutoCartesianView : public QObject
{
    Q_OBJECT

private slots:
    void parsePlan_readsTheBackendReply();
    void parsePlan_refusesAReplyWithoutAGrid();
    void parseCubes_listsTheOutputs();
    void mirror_isOnlyForAMeridionalImage();
    void summary_namesTheViewTheGridAndTheBox();
    void requestBody_carriesTheBudgetAndOnlyWhatIsAsked();
    void voxelBudget_isInMillions();
    void isCartesianViewPath_knowsTheBackendsFileName();
};
