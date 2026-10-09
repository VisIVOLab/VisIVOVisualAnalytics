#pragma once
// TestSpectralAxisInfer.h
// QTest class for the CTYPE→spectral-axis classifier lifted out of
// vtkWindowCube_Wcs.cpp (E-04). Header-only, VTK-free.

#include <QObject>

class TestSpectralAxisInfer : public QObject
{
    Q_OBJECT

private slots:
    void nonPhysical_isChannel();
    void freq_isFrequencyTrusted();
    void vrad_isRadioVelocity();
    void vopt_isOpticalVelocity();
    void felo_isOpticalVelocity();
    void velo_isGenericVelocityInferred();
    void containsVelo_isGenericVelocity();
    void chan_isChannelNonPhysical();
    void unknownNonEmpty_isGenericSpectralInferred();
    void unknownEmpty_isGenericSpectralNotInferred();
    void caseInsensitive_lowercaseFreq();
    void unitIsTrimmed();
    void branchOrder_feloBeforeVelo();
    void label_matchesKind();
};
