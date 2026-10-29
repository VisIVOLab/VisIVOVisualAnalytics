#ifndef SkavaSearchPanel_h
#define SkavaSearchPanel_h

#include "../skava/SkavaClient.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class OIDCAuthorizationCodeFlow;
class QAction;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableView;
class QToolButton;
class SkavaResultsModel;
class Settings;

/// Self-contained search-and-open panel for the SKAVA Data Discovery and
/// Access service. Lives in a tab inside the Data Hub, mirroring the
/// pattern used for VLKB.
///
/// Layout (top → bottom):
///
///   1. Connection bar      — protocol, base URL, token or OIDC sign-in,
///                            TAP / DataLink / broker URLs (sync'd with Settings)
///   2. Scientific filters  — POS / BAND / TIME / COLLECTION / DPTYPE /
///                            obs_id + pagination controls
///   3. Search button
///   4. Results table       — obs_id, target, RA/Dec, dptype, frequency
///                            range (auto-converted from m to MHz/GHz when
///                            sensible), MJD range, best node, latency
///   5. Provenance panel    — selected dataset's DOI / PID / citation /
///                            license (copy-to-clipboard buttons)
///   6. Open button         — resolves /datalink/{obs_id} and emits
///                            openSkavaDatasetRequested(obs_id, datalink);
///                            MainWindow routes it to the VisIVO backend on
///                            the node that holds the data.
///
/// All network activity runs on a QtConcurrent worker so the UI thread
/// stays responsive — the same async pattern used by the rest of the
/// desktop app.
class SkavaSearchPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SkavaSearchPanel(Settings *settings, QWidget *parent = nullptr);
    ~SkavaSearchPanel() override;

signals:
    /// Emitted when the user has clicked **Open** (or chose a node under
    /// **Open via…**, then ``preferredNode`` is its SKAVA code) and SKAVA
    /// resolved the full DataLink envelope. MainWindow uses this to make a
    /// backend-aware routing decision (compute next to data when a
    /// visivo-backend service descriptor matches a registered backend;
    /// otherwise fall back to download via the local backend).
    void openSkavaDatasetRequested(const QString &obsId,
                                    const SkavaDatalinkResponse &datalink,
                                    const QString &preferredNode);

    /// Emitted when the user chose an operation under **Compute next to
    /// data** and SKAVA resolved the DataLink envelope: ``kind`` is "cutout"
    /// (SODA subset), "moment", "pv" or "sourcefind". MainWindow prompts for
    /// the parameters, runs the operation on the dataset's node through the
    /// services SKAVA advertises, and opens the product in place.
    void nodeOperationRequested(const QString &kind, const QString &obsId,
                                const SkavaDatalinkResponse &datalink);

private slots:
    void runSearch();
    void onResultSelectionChanged();
    /// Resolve the selected row's access and ask MainWindow to open it;
    /// ``preferredNode`` (a SKAVA node code) asks for that node first.
    void openSelected(const QString &preferredNode = QString());
    /// Resolve the selected row's access and ask MainWindow for ``kind``
    /// (see nodeOperationRequested) on its node.
    void requestOperation(const QString &kind);
    void copyToClipboard();

private:
    /// Resolve ``row``'s access (the shared DataLink watcher), then emit the
    /// open signal, or ``nodeOperationRequested`` with ``operation``. The
    /// row is held, not re-read from the table, so a retry after a sign-in
    /// acts on the dataset the user asked about.
    void resolveRow(const SkavaDiscoveryResult &row, const QString &operation,
                    const QString &preferredNode);

private:
    void buildUi();
    void applySettingsToFields();
    /// A client configured from the connection bar, for one request.
    std::shared_ptr<SkavaClient> makeClient() const;
    void setBusy(bool busy, const QString &message = {});
    /// Ask the in-flight request to unwind; the watcher still clears the busy state.
    void cancelRequest();
    /// The sign-in flow from the settings' IAM endpoints; (re)built when they
    /// change. Null when none are set.
    OIDCAuthorizationCodeFlow *ensureOidc();
    /// The service answered 401: offer to sign in (configuring the IAM from
    /// the challenge's discovery document when the settings name none), then
    /// run ``retry``. Returns false when there is nothing to offer.
    bool offerSignIn(const SkavaAuthChallenge &challenge, std::function<void()> retry);
    void signInOrOut();
    void editSignInSettings();
    void onSignedIn();
    void onSignedOut();
    void updateSignInStatus();

    Settings *m_settings{ nullptr };
    /// The client of the request in flight, shared with its worker: the
    /// panel keeps it only to cancel it; the worker owns its lifetime.
    std::shared_ptr<SkavaClient> m_activeClient;
    /// The generation the request in flight was submitted under; a Cancel
    /// bumps the current one, and a completion of an older generation is
    /// not acted on.
    int m_submittedGeneration{ 0 };

    // Connection bar
    QComboBox *m_protocolCombo{ nullptr };   ///< SKAVA JSON / IVOA TAP + DataLink
    QLineEdit *m_urlEdit{ nullptr };
    QLineEdit *m_tokenEdit{ nullptr };
    QLineEdit *m_tapEdit{ nullptr };         ///< VO protocol: the TAP service
    QLineEdit *m_datalinkEdit{ nullptr };    ///< VO protocol: the DataLink service
    QLineEdit *m_brokerEdit{ nullptr };      ///< the Computing Broker, for the source finder
    QPushButton *m_signInBtn{ nullptr };
    QPushButton *m_signInSetupBtn{ nullptr };
    QLabel      *m_signInStatus{ nullptr };
    /// The OIDC sign-in to SKAVA, built from the settings when they name an
    /// IAM (or from the 401's own pointer to one); null until then.
    OIDCAuthorizationCodeFlow *m_oidc{ nullptr };
    QString m_oidcKey;   ///< the endpoints and client m_oidc was built for
    /// What to run again once the user has signed in, after a 401.
    std::function<void()> m_retryAfterSignIn;

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
    /// "Open via…": the selected dataset's replicas, each with SKAVA's reason,
    /// to open on a node other than the one SKAVA ranked first.
    QToolButton *m_openViaBtn{ nullptr };
    QMenu       *m_openViaMenu{ nullptr };
    /// "Compute next to data": the operations the dataset's node runs on it
    /// in place (SODA cutout, moment map, PV diagram, source finder).
    QToolButton *m_computeBtn{ nullptr };
    QAction     *m_cutoutAct{ nullptr };
    QAction     *m_momentAct{ nullptr };
    QAction     *m_pvAct{ nullptr };
    QAction     *m_finderAct{ nullptr };
    QLabel      *m_statusLabel{ nullptr };
    QProgressBar *m_busyBar{ nullptr };   ///< indeterminate, while a request is out
    bool m_busy{ false };                ///< a request is in flight (Search reads "Cancel")

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

    /// Set by ``requestOperation()`` so the shared DataLink watcher emits
    /// ``nodeOperationRequested`` with this kind instead of the open signal
    /// when the resolution completes. Reset on every datalink completion.
    QString m_pendingOperation;
    /// The node "Open via…" asked for, handed on with the open signal.
    QString m_pendingNode;
    /// The row being resolved (for a retry after a sign-in).
    SkavaDiscoveryResult m_pendingRow;
    /// Bumped by Cancel: a worker that ends later finds its generation
    /// gone and does not act on its result.
    int m_requestGeneration{ 0 };
};

#endif  // SkavaSearchPanel_h
