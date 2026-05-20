#pragma once

#include <QFrame>
#include <QPointer>

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

signals:
    /// Emitted when the user drags or arrows the overlay slider. The host
    /// (vtkWindowCube) forwards this to ui->spinSlice / the slice fetcher.
    void channelChanged(int channel);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void applyStyle();
    void repositionOverParent();

    QLabel  *m_chLabel   { nullptr };   // "CH"
    QLabel  *m_chValue   { nullptr };   // "128"
    QSlider *m_slider    { nullptr };
    QLabel  *m_vLabel    { nullptr };   // "v"
    QLabel  *m_vValue    { nullptr };   // "−297.4 km/s"
};
