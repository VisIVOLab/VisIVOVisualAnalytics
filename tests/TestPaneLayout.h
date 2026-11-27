#pragma once
// TestPaneLayout.h
// QTest class for the pane layout rules shared by the cube and the particle window
// (PaneLayoutModel.h) and the particle panes' widget-free state (ParticlePaneState.h).

#include <QObject>

class TestPaneLayout : public QObject
{
    Q_OBJECT

private slots:
    void normalisedCount_acceptsOnlyOneTwoFour();
    void visibleSlots_perLayout();
    void visibleSlots_twoPanesStackedUseTheLeftColumn();
    void columns_leftHoldsSlotsZeroAndTwo();
    void growth_goesOneTwoFour();
    void newPane_takesTheFirstFreeVisibleSlot();
    void newPane_growsWhenEverySlotIsTaken();
    void newPane_stackedPairGrowsIntoTheRightColumn();
    void newPane_refusedWhenFull();
    void survivor_activePaneMovesToSlotZero();

    void nextQuantity_prefersDensityThenTemperatureThenEntropy();
    void nextQuantity_skipsShownAndCoordinates();
    void nextQuantity_fallsBackToAnyColumn();
    void nextQuantity_noneLeft();

    void grid_detachDockMaximiseState();
    void grid_detachRefusedWhileMaximised();
    void grid_dockIsIdempotent();
    void grid_invalidSlotsAreRefused();
    void grid_panesPopOutIndependently();

    void pauseStream_onlyAHiddenPaneOfAVisibleGrid();

    void session_roundTrips();
    void session_toleratesGarbage();
    void session_countCoversThePaneList();
};
