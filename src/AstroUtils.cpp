#include "AstroUtils.h"

#include "FitsHeaderString.h"
#include "fitswcs.h"
#include "wcs.h"

#include <cmath>
#include <cstdio>

#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

namespace {
// libwcs parses headers through file-static state: hlength() stores the current
// header's length in `lhead0`, and every hget*/wcsinit call reads it back. Two
// AstroUtils built at the same time - which happens, the layer loader runs on a
// worker thread while the GUI thread can build its own - would each parse with
// the other's length: keywords missed, or a read past the end of the buffer.
// libwcs cannot be made re-entrant from here, so construction is serialised.
// It is short and bounded; nothing else in this process touches those calls.
QMutex &libwcsHeaderMutex()
{
    static QMutex mutex;
    return mutex;
}

QString formatAxisName(const char *ctype, int axisIndex)
{
    const QString ctypeString = QString::fromUtf8(ctype).trimmed();
    return ctypeString.isEmpty() ? QStringLiteral("AXIS%1").arg(axisIndex + 1) : ctypeString;
}

QString formatAxisValueLabel(const char *ctype, double value)
{
    const QString axisType = QString::fromUtf8(ctype).trimmed().toUpper();
    if (axisType == QStringLiteral("STOKES") && std::isfinite(value)) {
        switch (static_cast<int>(std::lround(value))) {
        case 1:
            return QStringLiteral("I");
        case 2:
            return QStringLiteral("Q");
        case 3:
            return QStringLiteral("U");
        case 4:
            return QStringLiteral("V");
        default:
            break;
        }
    }
    return QString::number(value, 'g', 10);
}
}

AstroUtils::AstroUtils(const std::string &filepath)
    : filepath{ filepath },
      wcs{ },
      naxis{ },
      secpix{ },
      bunit{ },
      naxes{ },
      crpix{ },
      crval{ },
      cdelt{ },
      ctype{ },
      cunit{ }
{
    const QMutexLocker locker(&libwcsHeaderMutex());
    this->wcs = GetWCSFITS(this->filepath.data(), 0);
    setwcsdeg(this->wcs, 1);

    if (!this->isSimulation()) {
        double ra1, dec1, ra2, dec2;
        pix2wcs(this->wcs, this->wcs->xrefpix - 0.5, this->wcs->yrefpix, &ra1, &dec1);
        pix2wcs(this->wcs, this->wcs->xrefpix + 0.5, this->wcs->yrefpix, &ra2, &dec2);
        this->secpix = 3600. * wcsdist(ra1, dec1, ra2, dec2);
    }

    int lhead = 0;
    int nbhead = 0;
    char *header = fitsrhead(this->filepath.data(), &lhead, &nbhead);
    this->readHeaderKeywords(header);
    free(header);
}

AstroUtils::AstroUtils(const std::vector<std::string> &headerCards)
    : filepath{ },
      wcs{ },
      naxis{ },
      secpix{ },
      bunit{ },
      naxes{ },
      crpix{ },
      crval{ },
      cdelt{ },
      ctype{ },
      cunit{ }
{
    // libwcs parses a header STRING: fixed 80-character card images, terminated
    // by END. That is exactly what the backend hands over, so nothing has to be
    // reformatted - only padded, since a card arrives trimmed.
    const QMutexLocker locker(&libwcsHeaderMutex());
    std::string header = visivo::buildFitsHeaderString(headerCards);

    // libwcs keeps the header length in a FILE-STATIC (hlength/gethlength), set
    // by whoever last read a header. Without setting it for THIS string, the
    // parse runs with another header's length - reading past the end of this
    // buffer, or stopping before its keywords.
    hlength(header.data(), static_cast<int>(header.size()));
    this->wcs = wcsinit(header.data());
    if (this->wcs) {
        setwcsdeg(this->wcs, 1);
        if (!this->isSimulation()) {
            double ra1, dec1, ra2, dec2;
            pix2wcs(this->wcs, this->wcs->xrefpix - 0.5, this->wcs->yrefpix, &ra1, &dec1);
            pix2wcs(this->wcs, this->wcs->xrefpix + 0.5, this->wcs->yrefpix, &ra2, &dec2);
            this->secpix = 3600. * wcsdist(ra1, dec1, ra2, dec2);
        }
    }
    hlength(header.data(), static_cast<int>(header.size()));
    this->readHeaderKeywords(header.data());
}

void AstroUtils::readHeaderKeywords(char *header)
{
    if (!header) {
        return;
    }
    hgeti4(header, "NAXIS", &this->naxis);
    hgets(header, "BUNIT", FLEN_VALUE, this->bunit);
    char key[FLEN_KEYWORD];
    for (int i = 1; i <= this->naxis && i <= 4; ++i) {
        std::snprintf(key, FLEN_KEYWORD, "NAXIS%d", i);
        hgeti4(header, key, &this->naxes[i - 1]);

        std::snprintf(key, FLEN_KEYWORD, "CRPIX%d", i);
        hgetr8(header, key, &this->crpix[i - 1]);

        std::snprintf(key, FLEN_KEYWORD, "CRVAL%d", i);
        hgetr8(header, key, &this->crval[i - 1]);

        std::snprintf(key, FLEN_KEYWORD, "CDELT%d", i);
        hgetr8(header, key, &this->cdelt[i - 1]);

        std::snprintf(key, FLEN_KEYWORD, "CTYPE%d", i);
        hgets(header, key, FLEN_VALUE, this->ctype[i - 1]);

        std::snprintf(key, FLEN_KEYWORD, "CUNIT%d", i);
        hgets(header, key, FLEN_VALUE, this->cunit[i - 1]);
    }
}

AstroUtils::~AstroUtils()
{
    wcsfree(this->wcs);
}

void AstroUtils::xy2sky(const double *pix, double *pos, int syswcs)
{
    if (nowcs(this->wcs)) {
        return;
    }

    char coorsys[80];
    wcscstr(coorsys, syswcs, 0., 0.);
    wcsoutinit(this->wcs, coorsys);
    pix2wcs(this->wcs, pix[0], pix[1], &pos[0], &pos[1]);
}

void AstroUtils::sky2xy(const double *pos, double *pix, int syswcs)
{
    if (nowcs(this->wcs)) {
        return;
    }

    char coorsys[80];
    int offscale = 0;
    wcscstr(coorsys, syswcs, 0., 0.);
    wcsininit(this->wcs, coorsys);
    wcs2pix(this->wcs, pos[0], pos[1], &pix[0], &pix[1], &offscale);
}

double AstroUtils::getSecPix() const
{
    return this->secpix;
}

std::string AstroUtils::getPhysicalUnit() const
{
    return this->bunit;
}

std::string AstroUtils::getAxisUnit(int axis) const
{
    if (axis < this->naxis) {
        return this->cunit[axis];
    }

    return { };
}

std::string AstroUtils::getAxisType(int axis) const
{
    if (axis < this->naxis) {
        return this->ctype[axis];
    }

    return { };
}

const int *AstroUtils::getDimensions() const
{
    return this->naxes;
}

const double *AstroUtils::getIncrements() const
{
    return this->cdelt;
}

const double *AstroUtils::getReferenceValues() const
{
    return this->crval;
}

const double *AstroUtils::getReferencePixels() const
{
    return this->crpix;
}

int AstroUtils::getAxisCount() const
{
    return this->naxis;
}

int AstroUtils::getActiveAxisCount() const
{
    int active = 0;
    for (int axis = 0; axis < this->naxis; ++axis) {
        if (this->naxes[axis] > 1) {
            ++active;
        }
    }
    return active;
}

QString AstroUtils::degenerateAxesSummary() const
{
    QStringList entries;
    for (int axis = 0; axis < this->naxis; ++axis) {
        if (this->naxes[axis] != 1) {
            continue;
        }
        const QString axisName = formatAxisName(this->ctype[axis], axis);
        QString entry = QStringLiteral("%1=%2").arg(axisName, formatAxisValueLabel(this->ctype[axis], this->crval[axis]));
        const QString unit = QString::fromUtf8(this->cunit[axis]).trimmed();
        if (!unit.isEmpty() && axisName.toUpper() != QStringLiteral("STOKES")) {
            entry += QStringLiteral(" %1").arg(unit);
        }
        entry += QStringLiteral(" (1)");
        entries.push_back(entry);
    }

    if (entries.isEmpty()) {
        return {};
    }
    return QStringLiteral("Collapsed axes: %1").arg(entries.join(QStringLiteral(", ")));
}

double AstroUtils::getInitialSpectralValue() const
{
    return this->crval[2] - this->cdelt[2] * (crpix[2] - 1);
}

void AstroUtils::getBounds(double *ra_min, double *ra_max, double *dec_min, double *dec_max,
                           int syswcs) const
{
    char coorsys[80];
    wcscstr(coorsys, syswcs, 0., 0.);
    wcsoutinit(this->wcs, coorsys);
    wcsrange(this->wcs, ra_min, ra_max, dec_min, dec_max);
}

SkyOverlap::Bounds AstroUtils::skyBounds() const
{
    SkyOverlap::Bounds bounds;
    this->getBounds(&bounds.raMin, &bounds.raMax, &bounds.decMin, &bounds.decMax, WCS_J2000);
    return bounds;
}

bool AstroUtils::overlap(const AstroUtils &other) const
{
    // No celestial solution on either side: there is no sky to share.
    if (this->isSimulation() || other.isSimulation()) {
        return false;
    }
    // The interval arithmetic lives in SkyOverlap so it can be tested on its
    // own — in particular the field that straddles RA = 0, which wcsrange()
    // reports as ra_min > ra_max and which a plain interval test rejects.
    return SkyOverlap::overlaps(this->skyBounds(), other.skyBounds());
}

bool AstroUtils::overlap(const std::string &filepath) const
{
    return this->overlap(AstroUtils(filepath));
}

bool AstroUtils::isImage() const
{
    return this->getActiveAxisCount() == 2;
}

bool AstroUtils::isCube() const
{
    return this->getActiveAxisCount() == 3;
}

bool AstroUtils::hasStokes() const
{
    return this->naxis == 4;
}

bool AstroUtils::isSimulation() const
{
    return nowcs(this->wcs) || this->wcs->syswcs == WCS_LINEAR || this->wcs->syswcs == WCS_XY;
}
