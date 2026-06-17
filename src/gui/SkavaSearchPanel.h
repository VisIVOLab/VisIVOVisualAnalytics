#ifndef SkavaSearchPanel_h
#define SkavaSearchPanel_h

#include "../skava/SkavaClient.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableView;
class SkavaResultsModel;
class Settings;

/// Self-contained search-and-open panel for the SKAVA Data Discovery and
/// Access service. Lives in a tab inside the Data Hub, mirroring the
/// pattern used for VLKB.
///
/// Layout (top → bottom):
///
///   1. Connection bar      — base URL + token (sync'd with Settings)
///   2. Scientific filters  — POS / BAND / TIME / COLLECTION / DPTYPE /
///                            obs_id + pagination controls
///   3. Search button
///   4. Results table       — obs_id, target, RA/Dec, dptype, frequency
///                            range (auto-converted from m to MHz/GHz when
///                            sensible), MJD range, best node, latency
///   5. Provenance panel    — selected dataset's DOI / PID / citation /
///                            license (copy-to-clipboard buttons)
///   6. Open button         — resolves /datalink/{obs_id} and emits
///                            openDatasetUrlRequested(url, obs_id), which
///                            the MainWindow forwards to the VisIVO backend
///                            `/v1/datasets/open_url` for download + open.
///
/// All network activity runs on a QtConcurrent worker so the UI thread
/// stays responsive — the same async pattern used by the rest of the
/// desktop app.
class SkavaSearchPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SkavaSearchPanel(Settings *settings,
                               class BackendClient *backendClient,
                               QWidget *parent = nullptr);
    ~SkavaSearchPanel() override;

signals:
    /// Legacy: emitted when only the access URL is needed. Still wired so
    /// callers that just want a download path keep working.
    void openDatasetUrlRequested(const QString &accessUrl, const QString &obsId);

    /// Emitted when the user has clicked **Open** and SKAVA resolved the
    /// full DataLink envelope. MainWindow uses this to make a
    /// backend-aware routing decision (compute next to data when a
    /// visivo-backend service descriptor matches a registered backend;
    /// otherwise fall back to download via the local backend).
    void openSkavaDatasetRequested(const QString &obsId,
                                    const SkavaDatalinkResponse &datalink);

    /// Emitted when the user clicked **SODA subset** and SKAVA resolved the
    /// DataLink envelope. MainWindow prompts for the POS/BAND region and runs
    /// the cutout on the dataset's co-located VisIVO backend (compute next to
    /// data), then opens the result in place. Distinct from the VLKB cutout
    /// flow, which is kept separately.
    void sodaSubsetRequested(const QString &obsId,
                              const SkavaDatalinkResponse &datalink);

private slots:
    void runSearch();
    void onResultSelectionChanged();
    void openSelected();
    void requestSubset();
    void copyToClipboard();

private:
    void buildUi();
    void applySettingsToFields();
    void reloadClientFromFields();
    void setBusy(bool busy, const QString &message = {});

    Settings *m_settings{ nullptr };
    BackendClient *m_backendClient{ nullptr };
    SkavaClient *m_skavaClient{ nullptr };

    // Connection bar
    QLineEdit *m_urlEdit{ nullptr };
    QLineEdit *m_tokenEdit{ nullptr };

    // Filters
    QLineEdit *m_raEdit{ nullptr };
    QLineEdit *m_decEdit{ nullptr };
    QLineEdit *m_radiusEdit{ nullptr };
    QLineEdit *m_freqMinEdit{ nullptr };
    QLineEdit *m_freqMaxEdit{ nullptr };
    QComboBox *m_freqUnit{ nullptr };
    QLineEdit *m_timeMinEdit{ nullptr };
    QLineEdit *m_timeMaxEdit{ nullptr };
    QLineEdit *m_collectionEdit{ nullptr };
    QComboBox *m_dptypeCombo{ nullptr };
    QLineEdit *m_obsIdEdit{ nullptr };
    QSpinBox  *m_limitSpin{ nullptr };
    QSpinBox  *m_offsetSpin{ nullptr };

    // Actions
    QPushButton *m_searchBtn{ nullptr };
    QPushButton *m_openBtn{ nullptr };
    QPushButton *m_subsetBtn{ nullptr };
    QLabel      *m_statusLabel{ nullptr };

    // Results
    QTableView         *m_resultsTable{ nullptr };
    SkavaResultsModel  *m_resultsModel{ nullptr };

    // Provenance
    QLabel      *m_provenanceTitle{ nullptr };
    QLabel      *m_provenanceDoi{ nullptr };
    QLabel      *m_provenancePid{ nullptr };
    QLabel      *m_provenanceLicense{ nullptr };
    QLabel      *m_provenanceCitation{ nullptr };
    QPushButton *m_copyCitationBtn{ nullptr };

    QFutureWatcher<SkavaDiscoveryResponse> *m_searchWatcher{ nullptr };
    QFutureWatcher<SkavaDatalinkResponse>  *m_datalinkWatcher{ nullptr };

    /// True between ``setBusy(true,…)`` and the matching
    /// ``setBusy(false)`` — used to keep the override-cursor stack
    /// balanced so a stuck WaitCursor never survives a request.
    bool m_cursorOverrideActive{ false };

    /// Set by ``requestSubset()`` so the shared DataLink watcher knows to
    /// emit ``sodaSubsetRequested`` instead of the open signals when the
    /// resolution completes. Reset on every datalink completion.
    bool m_pendingSubset{ false };
};

#endif  // SkavaSearchPanel_h
