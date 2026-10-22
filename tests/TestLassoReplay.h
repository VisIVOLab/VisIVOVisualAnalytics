#pragma once
// TestLassoReplay.h
// QTest class for LassoReplay — the lasso grow as the record a cube carries.
// Core-only; reads the service's request model from the repository.

#include <QObject>

class TestLassoReplay : public QObject
{
    Q_OBJECT

private slots:
    void growRequest_usesTheServiceRequestModelsFieldNames();
    void growRequest_sendsOptionalFieldsOnlyWhenSet();
    void applyStep_recordsTheModeAndTakesTheRoiFromTheGrowResponse();
    void fitStep_recordsItsOwnParametersAndTakesTheRoiFromTheGrowResponse();
    void historyAndDisplay_agreeWithTheRequest();
};
