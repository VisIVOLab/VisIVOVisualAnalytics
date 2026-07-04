#ifndef TestImageLod_h
#define TestImageLod_h

#include <QObject>

/// Unit tests for the viewport LOD math (ImageLod) used by 2-D tile streaming:
/// pyramid-level selection from zoom and visible-tile computation. Pure logic.
class TestImageLod : public QObject
{
    Q_OBJECT
private slots:
    void chooseLevel_fullResWhenZoomedIn();
    void chooseLevel_picksByLog2Ratio();
    void chooseLevel_clampsToNumLevels();
    void chooseLevel_guardsDegenerateViewport();

    void tileCounts_matchBackendGrid();

    void chooseAssemblyLevel_fullResWhenItFits();
    void chooseAssemblyLevel_capsHugeMosaic();
    void chooseAssemblyLevel_clampsAndGuards();

    void visibleTiles_singleTile();
    void visibleTiles_marginExpands();
    void visibleTiles_cornerOrderTolerant();
    void visibleTiles_clampsToImage();
    void visibleTiles_fullImageCoversAll();

    void viewportAssembly_fullResSubRegion();
    void viewportAssembly_wholeImageZoomedOut();
    void viewportAssembly_midLevelOriginAndSpacing();
    void viewportAssembly_coordinateMappingIsFullRes();
    void viewportAssembly_degenerateGridInvalid();
    void viewportAssembly_nonIntersectingInvalid();
};

#endif  // TestImageLod_h
