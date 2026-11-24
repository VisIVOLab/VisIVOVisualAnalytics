#pragma once
// TestAstroMatch.h
// QTest class for AstroUtils::pixelToPixel / matchView, the matcher behind the
// Link Views between an image and a cube (issue #522). The headers are built here as
// cards, as the backend's /v1/files/header hands them over; the expected
// numbers are astropy's (pixel_to_world / world_to_pixel on the same headers,
// frames transformed by astropy), so two grids of different scale, rotation,
// projection and frame are checked against an independent implementation.

#include <QObject>

class TestAstroMatch : public QObject
{
    Q_OBJECT

private slots:
    void pixelToPixelAgreesWithAstropy_data();
    void pixelToPixelAgreesWithAstropy();
    void matchViewKeepsCentreAndAngularHeight_data();
    void matchViewKeepsCentreAndAngularHeight();
    void matchViewThereAndBackIsTheIdentity();
    void aCentreOffTheProjectionIsRefused();
    void imagesWithoutSkyAreRefused();
};
