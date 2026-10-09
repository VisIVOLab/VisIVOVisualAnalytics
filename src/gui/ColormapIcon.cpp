#include "ColormapIcon.h"

#include <algorithm>

#include <vtkLookupTable.h>
#include <vtkNew.h>

#include <QImage>
#include <QPixmap>

#include "ColorMaps.h"

QIcon makeColormapIcon(const std::string &name, int iconW, int iconH)
{
    vtkNew<vtkLookupTable> lut;
    lut->SetNumberOfTableValues(256);
    lut->SetTableRange(0.0, 1.0);
    ColorMaps::SetColorMap(lut.Get(), name);
    lut->Build();
    QImage img(iconW, iconH, QImage::Format_RGB32);
    for (int x = 0; x < iconW; ++x) {
        // Sample via GetColor (maps through the table range) rather than
        // GetTableValue(idx): some colormaps (e.g. VolRenTwoLev) define only a
        // few entries, and a fixed 0..255 index would clamp to the last (grey).
        const double t = static_cast<double>(x) / std::max(1, iconW - 1);
        double rgb[3];
        lut->GetColor(t, rgb);
        const QRgb c = qRgb(static_cast<int>(rgb[0] * 255.0),
                            static_cast<int>(rgb[1] * 255.0),
                            static_cast<int>(rgb[2] * 255.0));
        for (int y = 0; y < iconH; ++y)
            img.setPixel(x, y, c);
    }
    return QIcon(QPixmap::fromImage(img));
}
