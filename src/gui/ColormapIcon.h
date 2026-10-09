#ifndef ColormapIcon_h
#define ColormapIcon_h

#include <string>

#include <QIcon>

// Gradient preview icon for a named colormap, shared by every viewer's colormap
// combo (image, cube, VBT, LUT customizer). Samples via vtkLookupTable::GetColor
// so colormaps with few table entries (e.g. VolRenTwoLev) render correctly.
QIcon makeColormapIcon(const std::string &name, int iconW = 80, int iconH = 14);

#endif
