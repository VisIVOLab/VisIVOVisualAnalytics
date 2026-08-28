#pragma once
// TestSpectralUnitConvert.h
// QTest class for the spectral-unit helpers (E-04 shared-helper lift).
// Header-only pure functions; no GUI/VTK.

#include <QObject>

class TestSpectralUnitConvert : public QObject
{
    Q_OBJECT

private slots:
    void parse_velocity_kms();
    void parse_velocity_ms();
    void parse_velocity_astropySpellings();
    void parse_frequency_all();
    void parse_mhzIsFrequencyNotVelocity();
    void parse_caseInsensitive();
    void parse_whitespaceStripped();
    void parse_unknownIsNotOk();

    void convert_kmsToMs();
    void convert_msToKms();
    void convert_frequencyHzToKhz();
    void convert_sameUnitIsIdentity();
    void convert_crossQuantityFails();
    void convert_unknownUnitFails();
};
