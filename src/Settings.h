#ifndef Settings_h
#define Settings_h

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>

class QSettings;

/// One entry in the desktop's known-backends registry.
///
/// In the distributed-backend architecture (see
/// docs/distributed-backend-design.md), each SKA Regional Centre or
/// data centre runs a VisIVO backend co-located with its dataset
/// filesystem. The desktop holds a list of all such backends; SKAVA's
/// DataLink response tells it which backend serves a given dataset.
///
/// The ``id == "local"`` entry is auto-managed by Settings and is the
/// default fallback whenever SKAVA does not declare a remote backend
/// for a dataset (preserving the legacy single-backend behaviour).
struct BackendNode
{
    QString id;       ///< Stable identifier, e.g. "local", "src-it-inaf"
    QString name;     ///< Human-readable name shown in UI
    QString url;      ///< Base URL (no trailing slash)
    QString token;    ///< Bearer token; empty for unauthenticated backends
    QString srcCode;  ///< Optional SKAVA Node code (matches the
                       ///< visivo-backend service descriptor's node_code)
    bool    isLocal{ false };    ///< True for the built-in local backend
    bool    isBuiltin{ false };  ///< Auto-managed entry; cannot be deleted
};

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

    // Cube-open pipeline tuning (Phase 3.6) — see Settings.cpp for the
    // QSettings keys + default values.
    int  getPreviewTargetMb() const;
    void setPreviewTargetMb(int mb);
    int  getLocalFullThresholdMb() const;
    void setLocalFullThresholdMb(int mb);

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
     * Zoom level the panoramic view opens at (OpenLayers level, 0 = whole sky).
     * @return the stored level, or the default when unset.
     */
    int getPanoramicZoom() const;
    void setPanoramicZoom(int zoom);

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

    // ── New-version notice (GitHub Releases) ─────────────────────────────────
    /// Whether the client asks GitHub for the latest release at startup (on by
    /// default; the request reveals the user's IP address to GitHub).
    bool getCheckForUpdates() const;
    void setCheckForUpdates(bool enabled);
    /// When the last automatic check completed; invalid if never.
    QDateTime getLastUpdateCheck() const;
    void setLastUpdateCheck(const QDateTime &when);
    /// A release the user chose to skip: the automatic check stays quiet about
    /// it (a manual check still reports it). Empty when none.
    QString getSkippedVersion() const;
    void setSkippedVersion(const QString &version);

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
   * @return The bearer token used to authenticate against SKAVA: the one
   * of the current sign-in when there is one (see setSkavaSessionToken),
   * else the stored one (SKAVA_ACCESS_TOKENS). Empty when SKAVA does not
   * require auth.
   */
    QString getSkavaToken() const;

    /**
   * Persist the SKAVA bearer token.
   * @param token Bearer token string
   */
    void setSkavaToken(const QString &token);

    /**
   * The token of an OpenID Connect sign-in to SKAVA: short-lived, refreshed
   * by the sign-in flow, kept in memory only (never in the settings file),
   * and read by every consumer through getSkavaToken() while it is set.
   * Empty = signed out.
   */
    void setSkavaSessionToken(const QString &token);
    QString getSkavaSessionToken() const { return m_skavaSessionToken; }

    /**
   * How the discovery tab talks to its service: ``"skava"`` (SKAVA's JSON
   * API, the default) or ``"vo"`` (IVOA standards: TAP on ObsCore and
   * DataLink VOTables — what the SRCNet global services speak).
   */
    QString getSkavaProtocol() const;
    void setSkavaProtocol(const QString &protocol);

    /**
   * TAP service base URL for the ``"vo"`` protocol (``…/tap``; the client
   * appends ``/sync``). Empty = ``<SKAVA url>/tap``.
   */
    QString getSkavaTapUrl() const;
    void setSkavaTapUrl(const QString &url);

    /**
   * DataLink endpoint for the ``"vo"`` protocol, used only when a TAP row
   * does not carry its own DataLink ``access_url`` (called as ``<url>?ID=``).
   * Empty = SKAVA's ``<SKAVA url>/datalink/<id>``.
   */
    QString getSkavaDatalinkUrl() const;
    void setSkavaDatalinkUrl(const QString &url);

    /**
   * Computing Broker (SRCNet-style job API) that places and runs a source
   * finder next to the data. Empty = run the finder as a task on the
   * dataset's node directly.
   */
    QString getSkavaBrokerUrl() const;
    void setSkavaBrokerUrl(const QString &url);

    /**
   * OIDC sign-in to SKAVA (the SRCNet IAM, or Keycloak on a testbed): the
   * authorization and token endpoints, the public client id and the scope.
   * Empty endpoints = no sign-in; the bearer token is pasted by hand.
   */
    QString getSkavaOidcAuthUrl() const;
    void setSkavaOidcAuthUrl(const QString &url);
    QString getSkavaOidcTokenUrl() const;
    void setSkavaOidcTokenUrl(const QString &url);
    QString getSkavaOidcClientId() const;
    void setSkavaOidcClientId(const QString &id);
    QString getSkavaOidcScope() const;
    void setSkavaOidcScope(const QString &scope);

    // ── Scientific copilot (LLM provider) ────────────────────────────────────
    // Persisted so the user configures the provider once. providerChoice mirrors
    // the panel/dialog combo: 0=Claude, 1=ChatGPT, 2=Your server, 3=None.
    int getCopilotProvider() const;
    void setCopilotProvider(int choice);
    QString getCopilotKey() const;        ///< the single key (cloud key or self-hosted key)
    void setCopilotKey(const QString &key);
    QString getCopilotUrl() const;        ///< self-hosted OpenAI-compatible base URL
    void setCopilotUrl(const QString &url);
    QString getCopilotModel() const;
    void setCopilotModel(const QString &model);

    // ── Multi-backend registry (distributed VisIVO backend) ──────────────────
    //
    // See docs/distributed-backend-design.md. The desktop holds a list of
    // known VisIVO backends keyed by ``id``. The ``"local"`` entry is
    // auto-managed and always present — it backs the legacy single-backend
    // getBackendUrl() / getBackendToken() accessors. Additional entries are
    // user-supplied (or, in the future, harvested via SKAVA's
    // visivo-backend service descriptors).

    /// All registered backends, including the auto-managed ``local`` entry.
    QList<BackendNode> getBackendNodes() const;
    /// Replace the entire registry. The ``local`` entry is preserved /
    /// reinserted if the caller omits it.
    void setBackendNodes(const QList<BackendNode> &nodes);
    /// Lookup by ``id``. Returns an entry with empty fields when missing.
    BackendNode getBackendNode(const QString &id) const;
    /// Convenience for the auto-managed ``local`` entry. Always returns a
    /// usable BackendNode (URL falls back to getBackendUrl() if storage is
    /// blank, e.g. on first launch).
    BackendNode getLocalBackend() const;
    /// Identifier of the backend the desktop uses when no SKAVA hint
    /// applies. Defaults to ``"local"``.
    QString getDefaultBackendId() const;
    void    setDefaultBackendId(const QString &id);
    /// Insert if absent (matched by ``id``); replace if present.
    void upsertBackendNode(const BackendNode &node);
    /// Remove an entry. The ``local`` entry cannot be removed.
    void removeBackendNode(const QString &id);

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
    QString m_skavaSessionToken;   ///< see setSkavaSessionToken()

    // Platform dependent
    std::pair<QString, QVariant> PythonLocation;
    void setPythonLocation();

    // Platform independent
    static const std::pair<QString, QVariant> ColorScheme;
    static const std::pair<QString, QVariant> PanoramicView;
    static const std::pair<QString, QVariant> PanoramicZoom;
    static const std::pair<QString, QVariant> MaxGlyphs;
    static const std::pair<QString, QVariant> VLKBUrl;
    static const std::pair<QString, QVariant> VLKBSearch;
    static const std::pair<QString, QVariant> UpdatesCheck;
    static const std::pair<QString, QVariant> UpdatesLastCheck;
    static const std::pair<QString, QVariant> UpdatesSkipped;

    // Backend settings (R1, R8)
    static const std::pair<QString, QVariant> BackendUrl;
    static const std::pair<QString, QVariant> BackendToken;

    // SKAVA discovery service
    static const std::pair<QString, QVariant> SkavaUrl;
    static const std::pair<QString, QVariant> SkavaToken;
    static const std::pair<QString, QVariant> SkavaProtocol;
    static const std::pair<QString, QVariant> SkavaTapUrl;
    static const std::pair<QString, QVariant> SkavaDatalinkUrl;
    static const std::pair<QString, QVariant> SkavaBrokerUrl;
    static const std::pair<QString, QVariant> SkavaOidcAuthUrl;
    static const std::pair<QString, QVariant> SkavaOidcTokenUrl;
    static const std::pair<QString, QVariant> SkavaOidcClientId;
    static const std::pair<QString, QVariant> SkavaOidcScope;

    // Scientific copilot (LLM provider)
    static const std::pair<QString, QVariant> CopilotProvider;
    static const std::pair<QString, QVariant> CopilotKey;
    static const std::pair<QString, QVariant> CopilotUrl;
    static const std::pair<QString, QVariant> CopilotModel;

    // Multi-backend registry (Backends/nodes is a JSON array of objects,
    // Backends/default is the chosen default backend id).
    static const std::pair<QString, QVariant> BackendNodes;
    static const std::pair<QString, QVariant> DefaultBackendId;
    /// Build the canonical ``local`` BackendNode from the legacy
    /// Backend/url + Backend/token settings. Always returns a usable entry.
    BackendNode buildLocalBackend() const;

    // Plot theme
    static const std::pair<QString, QVariant> PlotTheme;

    // Cube-open pipeline tuning (Phase 3.6).
    //
    // PreviewTargetMb:
    //   Target size in MB for the always-downloaded 3D preview. The
    //   client computes the downsample factor from this — small cubes
    //   come down at factor 1 (full data), big ones at higher factors.
    //   Lower on slow connections.
    //
    // LocalFullThresholdMb:
    //   Cube full-resolution size in MB at which the client stops
    //   auto-loading the full data locally and switches to remote
    //   WebRTC rendering. Above this the preview cube is kept as the
    //   instant placeholder and full quality comes from the V100
    //   stream. User can still override via the View menu.
    static const std::pair<QString, QVariant> PreviewTargetMb;
    static const std::pair<QString, QVariant> LocalFullThresholdMb;
};

#endif
