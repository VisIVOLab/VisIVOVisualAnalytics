#pragma once
// TestCatalogueCosmology.h
// QTest class for the client-side cosmology in Catalogue3DParser.h.
//
// The 3-D catalogue viewer computes Planck18 distances ITSELF — the combo's
// default is labelled "Planck18 (local)" — and asks the backend only for the
// other models. So the local integrator has to agree with the astropy Planck18
// the backend uses, or switching the combo from the default to another model
// and back would move every source for a reason that is not cosmology.
//
// The reference values are astropy 7's Planck18.comoving_distance(z) in Mpc.

#include <QObject>

class TestCatalogueCosmology : public QObject
{
    Q_OBJECT

private slots:
    void comovingDistanceMatchesAstropyPlanck18();
    void comovingDistanceIsZeroAtRedshiftZero();
    void negativeRedshiftsAreRefused();
    void comovingDistanceGrowsWithRedshift();
    void theHubbleLawIsTheLowRedshiftLimit();
    void theHubbleLawOverstatesDistanceAtHighRedshift();
};
