#pragma once
// TestProductRecipe.h
// QTest class for ProductRecipe — the exported record of an analysis product.
// Core-only; no widget.

#include <QObject>

class TestProductRecipe : public QObject
{
    Q_OBJECT

private slots:
    void replaySteps_carryTheRouteAndTheTypedRequestInOrder();
    void aStepWithoutARequest_isJustItsRoute();
    void aProductWithoutReplay_hasNoReplayKey();
    void volatileIds_areWrittenOnlyWhenKnown();
};
