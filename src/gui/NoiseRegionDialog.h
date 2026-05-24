#ifndef NoiseRegionDialog_h
#define NoiseRegionDialog_h

#include <QDialog>

class QPushButton;
class QSpinBox;

// ---------------------------------------------------------------------------
// NoiseRegionDialog
//
// Modal dialog that lets the user choose the spatial (x0,x1, y0,y1) and
// spectral (channelStart, channelEnd) region used for noise estimation.
// All fields are pre-filled with the full cube extent so the default
// behaviour is unchanged.  OK is disabled until the selection is valid.
// ---------------------------------------------------------------------------
class NoiseRegionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NoiseRegionDialog(int width, int height, int depth,
                               QWidget *parent = nullptr);

    // Call these only after exec() == QDialog::Accepted.
    int x0()          const;
    int x1()          const;
    int y0()          const;
    int y1()          const;
    int channelStart() const;
    int channelEnd()   const;

signals:
    // Emitted whenever any spinbox value changes (spatial or spectral) so
    // the cube viewer can paint live preview overlays: an amber rectangle
    // on the 2D slice (using x0..x1, y0..y1) AND a semi-transparent box
    // inside the 3D volume (using x0..x1, y0..y1, chStart..chEnd as the
    // X/Y/Z extent in pixel space, since the volume's Z axis is the
    // channel index).
    void regionPreviewChanged(int x0, int x1, int y0, int y1,
                              int chStart, int chEnd);

private:
    void validate();
    void resetToFullExtent();
    void emitRegion();

    const int m_width;
    const int m_height;
    const int m_depth;

    QSpinBox *m_x0Spin{ nullptr };
    QSpinBox *m_x1Spin{ nullptr };
    QSpinBox *m_y0Spin{ nullptr };
    QSpinBox *m_y1Spin{ nullptr };
    QSpinBox *m_chStartSpin{ nullptr };
    QSpinBox *m_chEndSpin{ nullptr };
    QPushButton *m_okBtn{ nullptr };
};

#endif // NoiseRegionDialog_h
