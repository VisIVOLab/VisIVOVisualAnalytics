#pragma once

#include <QFrame>
#include <QPointer>

class QFontMetrics;
class QLabel;
class QSlider;

/// Floating "channel scrubber" overlay anchored to the bottom of the 3D cube
/// viewport. Re-implements the design in cubeviewer.jsx:
///
///     ┌─────────────────────────────────────────────────────────────────────┐
///     │ CH  128   ──●─────────────────────── v  −297.4 km/s                  │
///     └─────────────────────────────────────────────────────────────────────┘
///
/// The overlay is a child of the cube QVTKOpenGLNativeWidget and re-positions
/// itself via an installed event filter on the parent's resize events. It
/// drives, and is driven by, the existing sidebar slice slider / spin box so
/// the two controls stay synchronised.
class ChannelOverlay : public QFrame
{
    Q_OBJECT

public:
    explicit ChannelOverlay(QWidget *parent = nullptr);

    /// Range and current channel are mirrored from the sidebar slice slider.
    void setRange(int minChannel, int maxChannel);
    void setChannel(int channel);                    // 1-based, like ui->spinSlice
    void setSpectralLabel(const QString &textWithUnit);

    int channel() const;

    /// The spectral value as shown in @p available pixels: the full text when it
    /// fits, else a frequency in GHz / MHz, else elided at the end — never cut at
    /// the front, which turned "1414222222.22 Hz" into a plausible "22.22 Hz".
    static QString fitSpectralLabel(const QString &full, const QFontMetrics &fm, int available);
    /// The symbol in front of the spectral value: ν (frequency), λ (wavelength)
    /// or v (velocity, or a bare channel), from the unit the value ends with.
    static QString spectralSymbol(const QString &valueWithUnit);
    /// Logical pixels the bar takes at the bottom of its host, margins included:
    /// what a 2-D view keeps free below its axes so the bar does not cover them.
    static int reservedHeight();

signals:
    /// Emitted when the user drags or arrows the overlay slider. The host
    /// (vtkWindowCube) forwards this to ui->spinSlice / the slice fetcher.
    void channelChanged(int channel);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyStyle();
    void refreshSpectralLabel();
    QString m_spectralFull;
    void repositionOverParent();

    QLabel  *m_chLabel   { nullptr };   // "CH"
    QLabel  *m_chValue   { nullptr };   // "128"
    QSlider *m_slider    { nullptr };
    QLabel  *m_vLabel    { nullptr };   // "v"
    QLabel  *m_vValue    { nullptr };   // "−297.4 km/s"
};
