#ifndef TestRenderExportPlan_h
#define TestRenderExportPlan_h

#include <QObject>

class TestRenderExportPlan : public QObject
{
    Q_OBJECT

private slots:
    void theCurrentSizeIsTheViews();
    void twiceAndFourfoldScaleBothSides();
    void aCustomWidthKeepsTheShapeOfTheView();
    void anEmptyViewHasNoPicture();
    void tooLargeASizeIsRefusedWithAReason();
    void theTileScaleCoversTheSize();
    void aNameWithoutOrWithAnotherSuffixGetsTheFormats();
    void theSuggestedNameIsSafeForAFileSystem();
};

#endif // TestRenderExportPlan_h
