#ifndef LinkedDatasetSky_h
#define LinkedDatasetSky_h

// LinkedDatasetSky.h
// The sky of a dataset for Link Views between an image window and a cube
// window (issue #522): the celestial WCS of the file, read once from the
// header cards the backend serves (so it works whether the file is on this
// machine or not) into AstroUtils, which then places a pixel of one dataset on
// another's (AstroUtils::pixelToPixel, matchView).

#include <QElapsedTimer>
#include <QString>

#include <memory>

class AstroUtils;

class LinkedDatasetSky
{
public:
    LinkedDatasetSky();
    ~LinkedDatasetSky();
    LinkedDatasetSky(const LinkedDatasetSky &) = delete;
    LinkedDatasetSky &operator=(const LinkedDatasetSky &) = delete;

    /// The dataset's sky; null when its header has no celestial solution (or
    /// could not be read). The header is fetched on the first call, with a wait
    /// cursor, and not again whether it gave a solution or not; a failed request
    /// is repeated after ten seconds.
    AstroUtils *get(const QString &backendUrl, const QString &token, const QString &sessionId,
                    const QString &filepath);

private:
    std::unique_ptr<AstroUtils> m_astro;
    bool m_read{ false };
    QElapsedTimer m_failedAt;   ///< since a header request failed; it is tried again after 10 s
};

#endif   // LinkedDatasetSky_h
