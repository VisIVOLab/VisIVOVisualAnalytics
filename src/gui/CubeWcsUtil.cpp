#include "CubeWcsUtil.h"

#include "wcs.h"  // libwcs: ra2str / dec2str / WCS_J2000 / WCS_GALACTIC / WCS_ECLIPTIC

#include <vtkAxisActor2D.h>
#include <vtkProperty2D.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <array>
#include <string>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace CubeWcs {

namespace {
// VTK renders text with its own font engine; its default (Arial-ish, faintly
// serif at small sizes) makes the 2-D overlays read as a different application
// from the Qt UI. Point VTK at the app's bundled Lato — but VTK wants a filesystem
// path, and our fonts live in the Qt resource bundle, so extract it once into the
// cache directory (§3.0c #8). No mono .ttf is bundled; Lato is the UI font.
const char *overlayFontFile()
{
    static const std::string path = []() -> std::string {
        const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cacheDir.isEmpty())
            return {};
        QDir().mkpath(cacheDir);
        const QString dest = cacheDir + u"/visivo-overlay-Lato-Regular.ttf"_s;
        if (!QFile::exists(dest)) {
            QFile src(u":/fonts/Lato-Regular.ttf"_s);
            if (!src.copy(dest))
                return {};
            QFile::setPermissions(dest, QFile::ReadOwner | QFile::WriteOwner);
        }
        return dest.toStdString();
    }();
    return path.c_str();
}
}  // namespace

void applyOverlayFont(vtkTextProperty *tp)
{
    if (!tp)
        return;
    const char *ff = overlayFontFile();
    if (ff && ff[0]) {
        tp->SetFontFamily(VTK_FONT_FILE);
        tp->SetFontFile(ff);
    }
}

void configureAxisActor(vtkAxisActor2D *axis, double x1, double y1, double x2, double y2)
{
    axis->GetPoint1Coordinate()->SetCoordinateSystemToDisplay();
    axis->GetPoint2Coordinate()->SetCoordinateSystemToDisplay();
    axis->GetPoint1Coordinate()->SetValue(x1, y1);
    axis->GetPoint2Coordinate()->SetValue(x2, y2);
    axis->SetNumberOfLabels(5);
    axis->AdjustLabelsOn();
    axis->SetLabelFormat("%-#6.4g");
    axis->SetFontFactor(0.6);
    axis->SetTickLength(6);
    axis->AxisVisibilityOn();
    axis->TickVisibilityOn();
    axis->LabelVisibilityOn();
    axis->TitleVisibilityOn();
    axis->GetProperty()->SetColor(1., 1., 1.);
    axis->GetLabelTextProperty()->SetColor(1., 1., 1.);
    axis->GetTitleTextProperty()->SetColor(1., 1., 1.);
    axis->GetLabelTextProperty()->SetFontSize(14);
    axis->GetTitleTextProperty()->SetFontSize(16);
    applyOverlayFont(axis->GetLabelTextProperty());
    applyOverlayFont(axis->GetTitleTextProperty());
}

void configureTickLabelActor(vtkTextActor *actor, bool rightAligned)
{
    actor->GetTextProperty()->SetColor(1., 1., 1.);
    actor->GetTextProperty()->SetFontSize(12);
    actor->GetTextProperty()->SetVerticalJustificationToCentered();
    if (rightAligned) {
        actor->GetTextProperty()->SetJustificationToRight();
    } else {
        actor->GetTextProperty()->SetJustificationToCentered();
    }
    applyOverlayFont(actor->GetTextProperty());
}

void configureVerticalAxisTitle(vtkAxisActor2D *axis)
{
    auto *titleProp = axis->GetTitleTextProperty();
    titleProp->SetOrientation(90.);
    titleProp->SetJustificationToCentered();
    titleProp->SetVerticalJustificationToCentered();
    titleProp->SetColor(1., 1., 1.);
    titleProp->SetFontSize(14);
    applyOverlayFont(titleProp);
    axis->SetTitlePosition(0.5);
}

QString formatCelestialCoordinate(int frame, int axis, double value)
{
    char buffer[64] = { 0 };
    if (frame == WCS_J2000 && axis == 0) {
        ra2str(buffer, sizeof(buffer), value, 1);
    } else {
        dec2str(buffer, sizeof(buffer), value, axis == 0 ? 1 : 0);
    }
    return QString::fromLatin1(buffer).trimmed();
}

QString upperCtype(const QString &value)
{
    return value.trimmed().toUpper();
}

int inferCelestialFrameFromCtypePair(const std::array<QString, 3> &ctype)
{
    const QString c1 = upperCtype(ctype[0]);
    const QString c2 = upperCtype(ctype[1]);
    if (c1.startsWith(u"GLON"_s) && c2.startsWith(u"GLAT"_s)) {
        return WCS_GALACTIC;
    }
    if (c1.startsWith(u"ELON"_s) && c2.startsWith(u"ELAT"_s)) {
        return WCS_ECLIPTIC;
    }
    if (c1.startsWith(u"RA"_s) && c2.startsWith(u"DEC"_s)) {
        return WCS_J2000;
    }
    return -1;
}

bool axisHasAnyMetadata(const QString &ctype, const QString &cunit, double crval, double crpix, double cdelt)
{
    return !ctype.trimmed().isEmpty() || !cunit.trimmed().isEmpty() || std::isfinite(crval)
            || std::isfinite(crpix) || std::isfinite(cdelt);
}

bool axisHasLinearWcs(const QString &ctype, double crval, double crpix, double cdelt)
{
    return !ctype.trimmed().isEmpty() && std::isfinite(crval) && std::isfinite(crpix)
            && std::isfinite(cdelt) && std::abs(cdelt) > 1e-12;
}

bool isCelestialLikeAxis(const QString &ctypeRaw)
{
    const QString ctype = ctypeRaw.trimmed().toUpper();
    return ctype.startsWith(u"RA"_s) || ctype.startsWith(u"DEC"_s) || ctype.startsWith(u"GLON"_s)
            || ctype.startsWith(u"GLAT"_s) || ctype.startsWith(u"ELON"_s)
            || ctype.startsWith(u"ELAT"_s);
}

}  // namespace CubeWcs
