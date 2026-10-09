#ifndef TestKinematicLevelField_h
#define TestKinematicLevelField_h

#include <QObject>

/// Thresholding of the Kinematic Lasso's geodesic level field.
///
/// The backend serves one grow up to a maximum budget as two uint8 fields, and
/// the desktop recovers every smaller budget by thresholding them locally, so
/// dragging Reach costs no round trip. The Python side already pins the geodesic
/// maths; what is untested is the CLIENT half — index arithmetic over two byte
/// arrays, where an axis-order or bbox slip produces a plausible-looking mask in
/// the wrong place rather than an error.
class TestKinematicLevelField : public QObject
{
    Q_OBJECT
private slots:
    // ── the contract the slider depends on ──────────────────────────────────
    void thresholdSelectsLevelsUpToTheBudget();
    void selectionGrowsMonotonicallyWithTheBudget();
    void seedLevelZeroIsSelectedAtBudgetZero();
    void unreachableIsNeverSelected();
    void negativeBudgetSelectsNothing();

    // ── carve seeds ─────────────────────────────────────────────────────────
    void subLevelsCarveAtTheSameBudget();
    void carveCanEmptyTheSelectionWithoutBeingAnError();
    void allSentinelSubFieldCarvesNothing();

    // ── geometry: the part that fails silently if it is wrong ───────────────
    void maskIsXFastestMatchingTheBackendLayout();
    void bboxIsReportedInFullCubeVoxels();
    void bboxCoversExactlyTheSelectedVoxels();
    void countMatchesTheNonZeroVoxelsInTheMask();

    // ── malformed input is rejected, not misread ────────────────────────────
    void rejectsMismatchedFieldSizes();
    void rejectsNonPositiveDimensions();
    void rejectsDimensionsThatWouldOverflowTheProduct();
    void rejectsAnOriginOutsideTheCube();
    void acceptsAnyOriginWhenNoFullShapeIsGiven();
};

#endif  // TestKinematicLevelField_h
