#ifndef AstroUtils_h
#define AstroUtils_h

#include "gui/SkyOverlap.h"

#include <fitsio.h>

#include <QString>

#include <array>
#include <string>
#include <vector>

struct WorldCoor;

/**
 * Util class to get FITS Header keywords and perform WCS conversions
 */
class AstroUtils
{
public:
    /**
   * Util class to get FITS Header keywords and perform WCS conversions
   * @param filepath FITS absolute filepath
   */
    AstroUtils(const std::string &filepath);
    /**
   * Build the WCS from FITS header CARDS instead of a file - the 80-character
   * card images the backend's ``/v1/files/header`` returns. A dataset that lives
   * on another machine can then be aligned to another one without copying a
   * single pixel to this one: the WCS is the only part of the file this class
   * ever needed.
   * @param headerCards one FITS card per entry, in header order
   */
    explicit AstroUtils(const std::vector<std::string> &headerCards);
    ~AstroUtils();

    /**
   * Convert pixel coordinates to World Coordinates
   * @param pix Pixel coordinates
   * @param pos World Coordinates (returned)
   * @param syswcs Output WCS
   */
    void xy2sky(const double *pix, double *pos, int syswcs);

    /**
   * Convert World Coordinates to pixel coordinates
   * @param pos World Coordinates
   * @param pix Pixel coordinates (returned)
   * @param syswcs Input WCS
   */
    void sky2xy(const double *pos, double *pix, int syswcs);

    /// What a 2-D view shows, in 0-based pixels of its own dataset: the pixel at
    /// the centre of the data area and how many pixels tall the area is.
    struct View
    {
        double cx{ 0.0 };
        double cy{ 0.0 };
        double height{ 0.0 };
    };

    /**
   * The pixel of @p to at the same sky position as pixel (px, py) of this
   * image, both 0-based (a world of the viewers) and through J2000, so two
   * images in different projections, frames and rotations match. A position
   * off @p to's image but within its projection is placed (the other view may
   * simply be panned past its edge); one off the projection is not.
   * @return false when either image has no celestial WCS or a position has no
   *         pixel on the projection.
   */
    bool pixelToPixel(AstroUtils &to, double px, double py, double &tx, double &ty);

    /**
   * How a small step in this image's pixels at (px, py) lands in @p to's: the
   * 2x2 matrix {a, b, c, d} (row-major) with d(tx) = a dpx + b dpy and
   * d(ty) = c dpx + d dpy, by a central difference of half a pixel. A shape's
   * size and angle go across with it, the position through pixelToPixel.
   */
    bool pixelJacobian(AstroUtils &to, double px, double py, std::array<double, 4> &m);

    /**
   * The view of @p to that shows the same sky as @p v of this image: the same
   * centre and the same angular height. North is not turned (a pan and a zoom,
   * as CARTA's XY matching does), so two images rotated against each other keep
   * their own orientation. False when the centre is not on @p to's projection.
   */
    bool matchView(AstroUtils &to, const View &v, View &out);

    /**
   * @return true if the header holds a celestial solution (not XY | LINEAR).
   */
    bool hasSky() const;

    /**
   * @return Arcseconds per pixel
   */
    double getSecPix() const;

    /**
   * @return BUNIT value or empty string
   */
    std::string getPhysicalUnit() const;

    /**
   * @param axis (0-index)
   * @return CUNIT value or empty string
   */
    std::string getAxisUnit(int axis) const;

    /**
   * @param axis (0-index)
   * @return CTYPE value or empty string
   */
    std::string getAxisType(int axis) const;

    /**
   * @return NAXIS values
   */
    const int *getDimensions() const;

    /**
   * @return CDELT values
   */
    const double *getIncrements() const;

    /**
   * @return CRVAL values
   */
    const double *getReferenceValues() const;

    /**
   * @return CRPIX values
   */
    const double *getReferencePixels() const;

    /**
   * @return FITS NAXIS keyword value
   */
    int getAxisCount() const;

    /**
   * @return number of axes with NAXISn > 1
   */
    int getActiveAxisCount() const;

    /**
   * @return summary of collapsed/singleton axes
   */
    QString degenerateAxesSummary() const;

    /**
   * @return Spectral value at first slice
   */
    double getInitialSpectralValue() const;

    /**
   * Return the bounds in World Coordinates
   * @param ra_min Right Ascension min value
   * @param ra_max Right Ascension max value
   * @param dec_min Declination min value
   * @param dec_max Declination max value
   * @param syswcs Output WCS
   */
    void getBounds(double *ra_min, double *ra_max, double *dec_min, double *dec_max,
                   int syswcs) const;

    /**
   * Check if two files overlap each other
   * @param other Absolute FITS filepath
   * @return true if there is an overlap
   */
    bool overlap(const std::string &filepath) const;

  /**
   * Check whether this image and @p other cover any common sky.
   *
   * The overload taking a path has to open a file; this one does not, so it
   * also serves the remote layer path, where both WCS are built from headers
   * fetched over HTTP and no file exists on this machine.
   *
   * @return true if there is an overlap
   */
    bool overlap(const AstroUtils &other) const;

  /**
   * @return the sky bounds used by overlap(), in degrees (J2000).
   */
    SkyOverlap::Bounds skyBounds() const;

    /**
   * @return true if FITS file has exactly 2 axes
   */
    bool isImage() const;

    /**
   * @return true if FITS file has at least 3 axes
   */
    bool isCube() const;

    /**
   * @return true if FITS file has 4 axes
   */
    bool hasStokes() const;

    /**
   * @return true if FITS file has no WCS keywords or WCS = XY | LINEAR
   */
    bool isSimulation() const;

private:
    /// The part of construction that is the same either way: everything after
    /// the WCS itself is read out of the header string with hget*.
    void readHeaderKeywords(char *header);

    std::string filepath;
    struct WorldCoor *wcs;
    int naxis;
    double secpix;
    char bunit[FLEN_VALUE];

    int naxes[4];
    double crpix[4];
    double crval[4];
    double cdelt[4];
    char ctype[4][FLEN_VALUE];
    char cunit[4][FLEN_VALUE];
};

#endif
