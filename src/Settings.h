#ifndef Settings_h
#define Settings_h

#include <QObject>
#include <QPointer>
#include <QVariant>

class QSettings;

/**
 * Application-level Settings class
 */
class Settings : public QObject
{
    Q_OBJECT

public:
    /**
   * Application-level Settings class
   * @param workDir Absolute directory path to store settings
   */
    explicit Settings(const QString &workDir, QObject *parent = nullptr);
    ~Settings() override;

    /**
   * @return Absolute directory path where settings are stored
   */
    QString getWorkingPath() const;

    /**
   * @return The application color scheme
   */
    Qt::ColorScheme getColorScheme() const;

    /**
   * Set the application color scheme
   * @param scheme The color scheme to apply
   */
    void setColorScheme(Qt::ColorScheme scheme);

    /**
   * @return Plot background theme: 0 = dark (default), 1 = light
   */
    int getPlotTheme() const;

    /**
   * Set plot background theme
   * @param theme 0 = dark, 1 = light
   */
    void setPlotTheme(int theme);

    /**
   * @return Python absolute path
   */
    QString getPythonLocation() const;

    /**
   * Set Python absolute path
   * @param location Python3 absolute path
   */
    void setPythonLocation(const QString &location);

    /**
   * @return Panoramic View URL
   */
    QString getPanoramicView() const;

    /**
   * Set Panoramic View URL
   * @param url Panoramic View URL
   */
    void setPanoramicView(const QString &url);

    /**
   * @return Maximum number of glyphs to visualize in vtkWindowSources
   */
    int getMaxGlyphs() const;

    /**
   * Set the maximum number of glyphs to visualize in vtkWindowSources
   * @param n Max number of glyphs
   */
    void setMaxGlyphs(int n);

    /**
   * @return Current VLKB services endpoint
   */
    QString getVLKBUrl() const;

    /**
   * Set VLKB endpoint
   * @param url VLKB endpoint URL
   */
    void setVLKBUrl(const QString &url);

    /**
   * @return true if VisIVO should search the Knowledge Base when loading local
   * data
   */
    bool getSearchOnImportFlag() const;

    /**
   * Enable or disable the Search on Import functionality
   * @param flag
   */
    void setSearchOnImportFlag(bool flag);

    // ── Backend (R1, R8) ──────────────────────────────────────────────────────

    /**
   * @return The VisIVO backend base URL (e.g. "http://127.0.0.1:8000")
   */
    QString getBackendUrl() const;

    /**
   * Set the VisIVO backend base URL
   * @param url Backend URL without trailing slash
   */
    void setBackendUrl(const QString &url);

    /**
   * @return The VisIVO backend auth token.
   * Empty means the token should be read from ~/.visivo_token or VISIVO_TOKEN env.
   */
    QString getBackendToken() const;

    /**
   * Persist the backend auth token to settings.
   * @param token The bearer token string
   */
    void setBackendToken(const QString &token);

    // ── SKAVA discovery service ───────────────────────────────────────────────

    /**
   * @return The SKAVA discovery service base URL
   * (e.g. "http://localhost:8000"). Empty when not configured.
   */
    QString getSkavaUrl() const;

    /**
   * Persist the SKAVA discovery base URL.
   * @param url base URL without trailing slash
   */
    void setSkavaUrl(const QString &url);

    /**
   * @return The bearer token used to authenticate against SKAVA
   * (SKAVA_ACCESS_TOKENS). Empty when SKAVA does not require auth.
   */
    QString getSkavaToken() const;

    /**
   * Persist the SKAVA bearer token.
   * @param token Bearer token string
   */
    void setSkavaToken(const QString &token);

public slots:
    /**
   * Reload settings from file
   */
    void reload();

    /**
   * Reset settings to default values
   */
    void resetDefaults();

signals:
    /**
   * Signal to notify consumer that they need to reload their settings
   */
    void updated();

private:
    const QString workDir;
    QPointer<QSettings> settings;

    // Platform dependent
    std::pair<QString, QVariant> PythonLocation;
    void setPythonLocation();

    // Platform independent
    static const std::pair<QString, QVariant> ColorScheme;
    static const std::pair<QString, QVariant> PanoramicView;
    static const std::pair<QString, QVariant> MaxGlyphs;
    static const std::pair<QString, QVariant> VLKBUrl;
    static const std::pair<QString, QVariant> VLKBSearch;

    // Backend settings (R1, R8)
    static const std::pair<QString, QVariant> BackendUrl;
    static const std::pair<QString, QVariant> BackendToken;

    // SKAVA discovery service
    static const std::pair<QString, QVariant> SkavaUrl;
    static const std::pair<QString, QVariant> SkavaToken;

    // Plot theme
    static const std::pair<QString, QVariant> PlotTheme;
};

#endif
