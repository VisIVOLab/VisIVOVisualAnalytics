#include "SkavaSearchPanel.h"

#include "../Settings.h"
#include "../auth/OIDCAuthorizationCodeFlow.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QSpinBox>
#include <QFont>
#include <QSplitter>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace {

constexpr double kSpeedOfLightMps = 299'792'458.0;

/// Wavelength (m) → frequency (Hz). Returns NaN if inputs invalid.
double wavelengthMtoHz(double m)
{
    return (m > 0.0 && std::isfinite(m)) ? kSpeedOfLightMps / m
                                           : std::numeric_limits<double>::quiet_NaN();
}

double frequencyHzToWavelengthM(double hz)
{
    return (hz > 0.0 && std::isfinite(hz)) ? kSpeedOfLightMps / hz
                                              : std::numeric_limits<double>::quiet_NaN();
}

/// Convert a frequency value entered in the UI to Hz, given the current
/// combo-box unit. Recognised: "Hz", "MHz", "GHz", "m".
double unitToHz(double value, const QString &unit)
{
    const QString u = unit.trimmed().toLower();
    if (u == "hz"_L1)   return value;
    if (u == "khz"_L1)  return value * 1e3;
    if (u == "mhz"_L1)  return value * 1e6;
    if (u == "ghz"_L1)  return value * 1e9;
    if (u == "m"_L1)    return wavelengthMtoHz(value);
    return value;
}

/// Format an em_min..em_max range (metres) into a human label.
/// Radio data show up as MHz/GHz, mid-IR as μm, etc.
QString formatEmRange(double emMin_m, double emMax_m)
{
    if (!std::isfinite(emMin_m) || !std::isfinite(emMax_m)) return {};
    const double fHi = frequencyHzToWavelengthM(emMin_m);  // shorter wavelength → higher freq
    const double fLo = frequencyHzToWavelengthM(emMax_m);
    if (!std::isfinite(fHi) || !std::isfinite(fLo)) return {};
    // Pick the unit that gives the most readable value
    const double mid = 0.5 * (fHi + fLo);
    if (mid >= 1e9) {
        return QStringLiteral("%1–%2 GHz").arg(fLo / 1e9, 0, 'f', 3)
                                           .arg(fHi / 1e9, 0, 'f', 3);
    } else if (mid >= 1e6) {
        return QStringLiteral("%1–%2 MHz").arg(fLo / 1e6, 0, 'f', 2)
                                           .arg(fHi / 1e6, 0, 'f', 2);
    } else if (emMin_m < 1e-4) {
        return QStringLiteral("%1–%2 μm").arg(emMin_m * 1e6, 0, 'f', 2)
                                          .arg(emMax_m * 1e6, 0, 'f', 2);
    }
    return QStringLiteral("%1–%2 m").arg(emMin_m, 0, 'g', 4)
                                     .arg(emMax_m, 0, 'g', 4);
}

QString formatPos(double ra, double dec)
{
    if (!std::isfinite(ra) || !std::isfinite(dec)) return {};
    return QStringLiteral("%1°, %2°").arg(ra, 0, 'f', 4).arg(dec, 0, 'f', 4);
}

QString formatMjdRange(double tMin, double tMax)
{
    if (!std::isfinite(tMin) || !std::isfinite(tMax)) return {};
    return QStringLiteral("MJD %1–%2").arg(tMin, 0, 'f', 2).arg(tMax, 0, 'f', 2);
}

/// The replica SKAVA ranked first: ``best_node``, else the first listed.
const SkavaReplica *bestReplica(const SkavaDiscoveryResult &r)
{
    for (const auto &rep : r.replicas) {
        if (rep.code == r.bestNode)
            return &rep;
    }
    return r.replicas.isEmpty() ? nullptr : &r.replicas.first();
}

/// One line per replica: the node, its cost and SKAVA's reason, in ranking
/// order, so the user sees the whole decision, not just its winner.
QString replicasTooltip(const SkavaDiscoveryResult &r)
{
    QStringList lines;
    for (const auto &rep : r.replicas) {
        QString line = rep.code == r.bestNode ? u"▶ %1"_s.arg(rep.code) : u"   %1"_s.arg(rep.code);
        if (!rep.name.isEmpty() && rep.name != rep.code)
            line += u" (%1)"_s.arg(rep.name);
        if (std::isfinite(rep.score))
            line += u", cost %1"_s.arg(rep.score, 0, 'g', 3);
        if (!rep.rankReason.isEmpty())
            line += u": %1"_s.arg(rep.rankReason);
        if (std::isfinite(rep.telemetryAgeS))
            line += u" (telemetry %1 s old)"_s.arg(rep.telemetryAgeS, 0, 'f', 0);
        if (!rep.isAvailable)
            line += u" [unavailable]"_s;
        lines << line;
    }
    return lines.join(u'\n');
}

}  // namespace

// ── Results model ─────────────────────────────────────────────────────────

class SkavaResultsModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        ColObsId = 0,
        ColTarget,
        ColType,
        ColPos,
        ColBand,
        ColTime,
        ColServedBy,
        ColReason,
        ColCount,
    };

    explicit SkavaResultsModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setResults(const QVector<SkavaDiscoveryResult> &results)
    {
        beginResetModel();
        m_rows = results;
        endResetModel();
    }
    const QVector<SkavaDiscoveryResult> &results() const { return m_rows; }

    int rowCount(const QModelIndex & = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return ColCount; }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid()
            || index.row() < 0 || index.row() >= m_rows.size())
            return {};
        const auto &r = m_rows[index.row()];
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case ColObsId:    return r.metadata.obsId;
            case ColTarget:   return r.metadata.targetName;
            case ColType:     return r.metadata.dataproductType;
            case ColPos:      return formatPos(r.metadata.sRa, r.metadata.sDec);
            case ColBand:     return formatEmRange(r.metadata.emMin, r.metadata.emMax);
            case ColTime:     return formatMjdRange(r.metadata.tMin, r.metadata.tMax);
            case ColServedBy: {
                // The node SKAVA ranked first for this dataset, and how many
                // other replicas it could fall back to.
                if (r.bestNode.isEmpty())
                    return u"—"_s;
                const int others = std::max(0, static_cast<int>(r.replicas.size()) - 1);
                return others > 0 ? u"%1 (+%2)"_s.arg(r.bestNode).arg(others) : r.bestNode;
            }
            case ColReason: {
                // Why: the policy's reason for the serving node. The static
                // policy's reason names declared scores; the resource-aware
                // one measured values.
                const SkavaReplica *best = bestReplica(r);
                return best && !best->rankReason.isEmpty() ? best->rankReason : u"—"_s;
            }
            default: return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            if (index.column() == ColObsId && !r.metadata.obsTitle.isEmpty())
                return r.metadata.obsTitle;
            if (index.column() == ColServedBy || index.column() == ColReason)
                return replicasTooltip(r);
            // Any other cell may be elided: its full text.
            return data(index, Qt::DisplayRole);
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case ColObsId:    return u"obs_id"_s;
        case ColTarget:   return u"Target"_s;
        case ColType:     return u"Type"_s;
        case ColPos:      return u"Sky position"_s;
        case ColBand:     return u"Band"_s;
        case ColTime:     return u"Time"_s;
        case ColServedBy: return u"Served by"_s;
        case ColReason:   return u"Why"_s;
        default:          return {};
        }
    }

private:
    QVector<SkavaDiscoveryResult> m_rows;
};


// ── Constructor / build UI ─────────────────────────────────────────────────

SkavaSearchPanel::SkavaSearchPanel(Settings *settings, QWidget *parent)
    : QWidget(parent),
      m_settings(settings),
      m_resultsModel(new SkavaResultsModel(this)),
      m_searchWatcher(new QFutureWatcher<SkavaDiscoveryResponse>(this)),
      m_datalinkWatcher(new QFutureWatcher<SkavaDatalinkResponse>(this))
{
    buildUi();
    applySettingsToFields();

    QObject::connect(m_searchWatcher, &QFutureWatcher<SkavaDiscoveryResponse>::finished,
                     this, [this]() {
        setBusy(false);
        m_activeClient.reset();
        if (m_submittedGeneration != m_requestGeneration)
            return;   // cancelled: whatever came back is not acted on
        const auto resp = m_searchWatcher->result();
        if (!resp.valid) {
            // A 401: sign in and search again, when an IAM is at hand.
            if (resp.authChallenge.required
                && offerSignIn(resp.authChallenge, [this]() { runSearch(); }))
                return;
            QMessageBox::warning(this, u"SKAVA Discovery"_s,
                    u"Search failed:\n\n%1"_s.arg(
                            resp.error.isEmpty() ? u"unknown error"_s : resp.error));
            return;
        }
        m_resultsModel->setResults(resp.results);
        m_statusLabel->setText(
                QStringLiteral("Showing %1 of %2 results (limit=%3, offset=%4)")
                        .arg(resp.returned).arg(resp.total)
                        .arg(resp.limit).arg(resp.offset));
        m_openBtn->setEnabled(false);
        if (m_computeBtn) m_computeBtn->setEnabled(false);
    });

    QObject::connect(m_datalinkWatcher, &QFutureWatcher<SkavaDatalinkResponse>::finished,
                     this, [this]() {
        setBusy(false);
        m_activeClient.reset();
        // Capture + clear the intent up front so a failed resolution can't
        // leave a stale operation (or node) that hijacks the next Open.
        const QString operation = m_pendingOperation;
        const QString preferredNode = m_pendingNode;
        const SkavaDiscoveryResult row = m_pendingRow;
        m_pendingOperation.clear();
        m_pendingNode.clear();
        m_pendingRow = {};
        if (m_submittedGeneration != m_requestGeneration)
            return;   // cancelled: whatever came back is not acted on
        auto resp = m_datalinkWatcher->result();
        if (!resp.valid) {
            // A 401: sign in and resolve the same action, on the same row,
            // again (the selection may have moved meanwhile).
            if (resp.authChallenge.required
                && offerSignIn(resp.authChallenge, [this, row, operation, preferredNode]() {
                       resolveRow(row, operation, preferredNode);
                   }))
                return;
            QMessageBox::warning(this, u"SKAVA DataLink"_s,
                    u"Access resolution failed:\n\n%1"_s.arg(
                            resp.error.isEmpty() ? u"unknown error"_s : resp.error));
            return;
        }
        // resolveAccess() already merged the selected row's full ObsCore metadata.
        // The same DataLink resolution feeds every action: an operation next
        // to the data goes to its flow, otherwise this is the normal open.
        if (!operation.isEmpty()) {
            emit nodeOperationRequested(operation, resp.dataset.obsId, resp);
            return;
        }
        // Backend-aware: MainWindow picks a registered backend from the
        // visivo-backend descriptors, the preferred node first.
        emit openSkavaDatasetRequested(resp.dataset.obsId, resp, preferredNode);
    });

    QObject::connect(m_resultsTable->selectionModel(),
                     &QItemSelectionModel::currentRowChanged,
                     this, &SkavaSearchPanel::onResultSelectionChanged);
    QObject::connect(m_searchBtn, &QPushButton::clicked, this, [this]() {
        if (m_busy)
            cancelRequest();
        else
            runSearch();
    });
    QObject::connect(m_openBtn, &QPushButton::clicked, this, [this]() { openSelected(); });
    QObject::connect(m_resultsTable, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.isValid() && !m_busy)
            openSelected();
    });
    QObject::connect(m_cutoutAct, &QAction::triggered, this, [this]() { requestOperation(u"cutout"_s); });
    QObject::connect(m_momentAct, &QAction::triggered, this, [this]() { requestOperation(u"moment"_s); });
    QObject::connect(m_pvAct, &QAction::triggered, this, [this]() { requestOperation(u"pv"_s); });
    QObject::connect(m_finderAct, &QAction::triggered, this, [this]() { requestOperation(u"sourcefind"_s); });
    QObject::connect(m_copyCitationBtn, &QPushButton::clicked,
                     this, &SkavaSearchPanel::copyToClipboard);
    QObject::connect(m_urlEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_settings) m_settings->setSkavaUrl(m_urlEdit->text().trimmed());
    });
    QObject::connect(m_tokenEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_settings)
            return;
        // The field shows the sign-in's token too; that one is never written
        // to the settings file. A token the user typed over it is: it takes
        // the place of the sign-in's from now on.
        const QString text = m_tokenEdit->text().trimmed();
        const QString session = m_settings->getSkavaSessionToken();
        if (!session.isEmpty() && text == session)
            return;
        m_settings->setSkavaToken(text);
        if (!session.isEmpty()) {
            // A token typed over the sign-in's ends the sign-in: otherwise
            // the flow's next refresh would put its own token back.
            m_settings->setSkavaSessionToken(QString());
            if (m_oidc && m_oidc->hasTokens())
                m_oidc->logout();          // onSignedOut shows the typed token
            updateSignInStatus();
        }
    });
    QObject::connect(m_protocolCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString protocol = m_protocolCombo->itemData(index).toString();
        if (m_settings) m_settings->setSkavaProtocol(protocol);
        const bool vo = protocol == "vo"_L1;
        m_tapEdit->setEnabled(vo);
        m_datalinkEdit->setEnabled(vo);
    });
    QObject::connect(m_tapEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_settings) m_settings->setSkavaTapUrl(m_tapEdit->text().trimmed());
    });
    QObject::connect(m_datalinkEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_settings) m_settings->setSkavaDatalinkUrl(m_datalinkEdit->text().trimmed());
    });
    QObject::connect(m_brokerEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_settings) m_settings->setSkavaBrokerUrl(m_brokerEdit->text().trimmed());
    });
    QObject::connect(m_signInBtn, &QPushButton::clicked, this, &SkavaSearchPanel::signInOrOut);
    QObject::connect(m_signInSetupBtn, &QPushButton::clicked, this, &SkavaSearchPanel::editSignInSettings);
    // An IAM named in the settings: build the flow now, so a sign-in kept
    // from an earlier session is picked up silently.
    ensureOidc();
    updateSignInStatus();
}

SkavaSearchPanel::~SkavaSearchPanel()
{
    // A worker still out keeps its client alive; it is told to stop.
    if (m_activeClient)
        m_activeClient->requestCancel();
}

namespace {

// A titled section whose body folds away behind a disclosure arrow, so the
// panel keeps its room for the results.
QWidget *makeFoldable(const QString &title, QWidget *body, bool expanded, QWidget *parent,
                      QToolButton **toggleOut = nullptr)
{
    auto *section = new QWidget(parent);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *toggle = new QToolButton(section);
    toggle->setText(title);
    toggle->setCheckable(true);
    toggle->setChecked(expanded);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    toggle->setAutoRaise(true);
    QFont f = toggle->font();
    f.setBold(true);
    toggle->setFont(f);
    layout->addWidget(toggle);
    layout->addWidget(body);
    body->setVisible(expanded);
    QObject::connect(toggle, &QToolButton::toggled, body, [toggle, body](bool on) {
        toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        body->setVisible(on);
    });
    if (toggleOut)
        *toggleOut = toggle;
    return section;
}

// A grid whose field columns carry the stretch: on macOS a QFormLayout kept
// its fields at their size hint however much room the row had.
QGridLayout *makeGrid(QWidget *owner)
{
    auto *grid = new QGridLayout(owner);
    grid->setContentsMargins(6, 4, 6, 4);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    return grid;
}

QLabel *rightLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

}  // namespace

void SkavaSearchPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ── Connection: the service and the identity, two rows ──────────
    {
        auto *box = new QGroupBox(u"Discovery service"_s, this);
        auto *grid = makeGrid(box);
        grid->setColumnStretch(2, 1);
        // Which interface the service speaks: SKAVA's own JSON, or the IVOA
        // standards the SRCNet global services speak.
        m_protocolCombo = new QComboBox(box);
        m_protocolCombo->addItem(u"SKAVA JSON API"_s, u"skava"_s);
        m_protocolCombo->addItem(u"IVOA TAP + DataLink (SRCNet)"_s, u"vo"_s);
        m_protocolCombo->setToolTip(u"SKAVA's JSON API, or the IVOA standards (TAP on ObsCore and DataLink "
                                    u"VOTables) any SRCNet discovery service speaks."_s);
        m_urlEdit = new QLineEdit(box);
        m_urlEdit->setPlaceholderText(u"SKAVA URL, e.g. http://localhost:8000"_s);
        m_urlEdit->setToolTip(u"The SKAVA (or SRCNet discovery) service to search."_s);
        grid->addWidget(rightLabel(u"Service"_s, box), 0, 0);
        grid->addWidget(m_protocolCombo, 0, 1);
        grid->addWidget(m_urlEdit, 0, 2);

        // The token: pasted, or obtained by signing in to the IAM.
        m_tokenEdit = new QLineEdit(box);
        m_tokenEdit->setPlaceholderText(u"not needed by an open service; paste a bearer token or sign in"_s);
        m_tokenEdit->setEchoMode(QLineEdit::Password);
        m_signInBtn = new QPushButton(u"Sign in…"_s, box);
        m_signInBtn->setToolTip(u"Sign in to the IAM the service trusts (OpenID Connect); the token "
                                u"it issues is used from then on."_s);
        m_signInSetupBtn = new QPushButton(u"IAM…"_s, box);
        m_signInSetupBtn->setToolTip(u"The IAM's endpoints and this client's id. A service that answers "
                                     u"401 with an ivoa_bearer challenge fills them in by itself."_s);
        m_signInStatus = new QLabel(box);
        auto *signRow = new QHBoxLayout;
        signRow->setSpacing(6);
        signRow->addWidget(m_signInBtn);
        signRow->addWidget(m_signInSetupBtn);
        signRow->addWidget(m_signInStatus);
        grid->addWidget(rightLabel(u"Token"_s, box), 1, 0);
        grid->addWidget(m_tokenEdit, 1, 1, 1, 2);
        grid->addLayout(signRow, 1, 3);

        // The endpoints a default deployment never needs to set.
        auto *advanced = new QWidget(box);
        auto *agrid = makeGrid(advanced);
        agrid->setContentsMargins(0, 0, 0, 0);
        agrid->setColumnStretch(1, 1);
        m_tapEdit = new QLineEdit(advanced);
        m_tapEdit->setPlaceholderText(u"empty: <service URL>/tap (IVOA protocol only)"_s);
        m_tapEdit->setToolTip(u"The TAP service (…/tap; /sync is appended), for the IVOA protocol."_s);
        m_datalinkEdit = new QLineEdit(advanced);
        m_datalinkEdit->setPlaceholderText(u"empty: <service URL>/datalink/<id> (IVOA protocol only)"_s);
        m_datalinkEdit->setToolTip(u"The DataLink service (called as <URL>?ID=…) when a TAP row carries "
                                   u"no DataLink access_url of its own."_s);
        m_brokerEdit = new QLineEdit(advanced);
        m_brokerEdit->setPlaceholderText(u"empty: the source finder runs as a task on the dataset's node"_s);
        m_brokerEdit->setToolTip(u"A Computing Broker (SRCNet job API) that places and runs the source "
                                 u"finder next to the data."_s);
        agrid->addWidget(rightLabel(u"TAP URL"_s, advanced), 0, 0);
        agrid->addWidget(m_tapEdit, 0, 1);
        agrid->addWidget(rightLabel(u"DataLink URL"_s, advanced), 1, 0);
        agrid->addWidget(m_datalinkEdit, 1, 1);
        agrid->addWidget(rightLabel(u"Broker URL"_s, advanced), 2, 0);
        agrid->addWidget(m_brokerEdit, 2, 1);
        grid->addWidget(makeFoldable(u"Advanced: TAP, DataLink, Computing Broker"_s, advanced, false, box),
                        2, 0, 1, 4);
        root->addWidget(box);
    }

    // ── Scientific filters: three rows, folded away when not needed ──
    {
        auto *body = new QWidget(this);
        auto *grid = makeGrid(body);
        for (int c : { 1, 3, 5 })
            grid->setColumnStretch(c, 1);

        auto dbl = [this](QLineEdit *e, double lo, double hi, int dec) {
            e->setValidator(new QDoubleValidator(lo, hi, dec, this));
        };
        // Position
        m_raEdit = new QLineEdit(body); m_raEdit->setPlaceholderText(u"RA (deg), e.g. 187.5"_s);
        dbl(m_raEdit, 0.0, 360.0, 6);
        m_decEdit = new QLineEdit(body); m_decEdit->setPlaceholderText(u"Dec (deg), e.g. 5.0"_s);
        dbl(m_decEdit, -90.0, 90.0, 6);
        m_radiusEdit = new QLineEdit(body); m_radiusEdit->setPlaceholderText(u"radius (deg), e.g. 1.0"_s);
        dbl(m_radiusEdit, 0.0, 180.0, 6);
        grid->addWidget(rightLabel(u"Position"_s, body), 0, 0);
        grid->addWidget(m_raEdit, 0, 1);
        grid->addWidget(m_decEdit, 0, 3);
        grid->addWidget(m_radiusEdit, 0, 5);

        // Band and time
        m_freqMinEdit = new QLineEdit(body); m_freqMinEdit->setPlaceholderText(u"band min, e.g. 900"_s);
        dbl(m_freqMinEdit, 0.0, 1e18, 6);
        m_freqMaxEdit = new QLineEdit(body); m_freqMaxEdit->setPlaceholderText(u"band max, e.g. 1670"_s);
        dbl(m_freqMaxEdit, 0.0, 1e18, 6);
        m_freqUnit = new QComboBox(body);
        m_freqUnit->addItems(QStringList{ u"MHz"_s, u"GHz"_s, u"Hz"_s, u"m"_s });
        auto *bandRow = new QHBoxLayout;
        bandRow->setSpacing(4);
        bandRow->addWidget(m_freqMaxEdit, 1);
        bandRow->addWidget(m_freqUnit);
        m_timeMinEdit = new QLineEdit(body); m_timeMinEdit->setPlaceholderText(u"MJD min, e.g. 59000"_s);
        dbl(m_timeMinEdit, 0.0, 1e9, 4);
        m_timeMaxEdit = new QLineEdit(body); m_timeMaxEdit->setPlaceholderText(u"MJD max, e.g. 60000"_s);
        dbl(m_timeMaxEdit, 0.0, 1e9, 4);
        auto *timeRow = new QHBoxLayout;
        timeRow->setSpacing(4);
        timeRow->addWidget(m_timeMinEdit, 1);
        timeRow->addWidget(m_timeMaxEdit, 1);
        grid->addWidget(rightLabel(u"Band"_s, body), 1, 0);
        grid->addWidget(m_freqMinEdit, 1, 1);
        grid->addLayout(bandRow, 1, 3);
        grid->addWidget(rightLabel(u"Time"_s, body), 1, 4);
        grid->addLayout(timeRow, 1, 5);

        // Collection, type, id, paging
        m_collectionEdit = new QLineEdit(body);
        m_collectionEdit->setPlaceholderText(u"collection, e.g. MGCLS"_s);
        m_dptypeCombo = new QComboBox(body);
        m_dptypeCombo->addItems(QStringList{ u""_s, u"image"_s, u"cube"_s,
                                              u"timeseries"_s, u"spectrum"_s });
        m_dptypeCombo->setToolTip(u"Data product type (empty: any)."_s);
        m_obsIdEdit = new QLineEdit(body);
        m_obsIdEdit->setPlaceholderText(u"obs_id, exact match"_s);
        m_limitSpin = new QSpinBox(body);
        m_limitSpin->setRange(1, 1000);
        m_limitSpin->setValue(100);
        m_limitSpin->setPrefix(u"limit "_s);
        m_offsetSpin = new QSpinBox(body);
        m_offsetSpin->setRange(0, 1'000'000);
        m_offsetSpin->setValue(0);
        m_offsetSpin->setPrefix(u"offset "_s);
        // Type and paging share a cell; the obs_id gets one of its own, so it
        // is never squeezed to a few characters.
        auto *typeRow = new QHBoxLayout;
        typeRow->setSpacing(4);
        typeRow->addWidget(new QLabel(u"type"_s, body));
        typeRow->addWidget(m_dptypeCombo, 1);
        typeRow->addWidget(m_limitSpin);
        typeRow->addWidget(m_offsetSpin);
        grid->addWidget(rightLabel(u"Dataset"_s, body), 2, 0);
        grid->addWidget(m_collectionEdit, 2, 1);
        grid->addLayout(typeRow, 2, 3);
        grid->addWidget(m_obsIdEdit, 2, 5);

        root->addWidget(makeFoldable(u"Filters (ObsCore; all optional)"_s, body, true, this));
    }

    // ── Action bar ───────────────────────────────────────────────────
    {
        auto *bar = new QHBoxLayout;
        m_searchBtn = new QPushButton(u"Search"_s, this);
        m_searchBtn->setDefault(true);
        m_openBtn   = new QPushButton(u"Open"_s, this);
        m_openBtn->setToolTip(u"Open the selected dataset in VisIVO, on the node SKAVA chose "
                              u"(double-click a row does the same)."_s);
        m_openBtn->setEnabled(false);
        // The replicas SKAVA listed, in its ranking order with its reasons:
        // the user can overrule the policy and open on another node.
        m_openViaBtn = new QToolButton(this);
        m_openViaBtn->setText(u"Open via…"_s);
        m_openViaBtn->setPopupMode(QToolButton::InstantPopup);
        m_openViaBtn->setToolTip(u"Open the selected dataset on a node of your choice "
                                 u"among those holding a replica."_s);
        m_openViaMenu = new QMenu(m_openViaBtn);
        m_openViaBtn->setMenu(m_openViaMenu);
        m_openViaBtn->setEnabled(false);
        // What the dataset's node computes on it in place, through the
        // services SKAVA advertises: the product opens in VisIVO without the
        // dataset ever leaving the node.
        m_computeBtn = new QToolButton(this);
        m_computeBtn->setText(u"Compute next to data…"_s);
        m_computeBtn->setPopupMode(QToolButton::InstantPopup);
        m_computeBtn->setEnabled(false);
        auto *computeMenu = new QMenu(m_computeBtn);
        m_cutoutAct = computeMenu->addAction(u"SODA cutout…"_s);
        m_cutoutAct->setToolTip(u"A spatial / spectral / temporal subset, cut on the node and opened in place."_s);
        m_momentAct = computeMenu->addAction(u"Moment map…"_s);
        m_momentAct->setToolTip(u"A moment map of the cube, computed on the node and opened in place."_s);
        m_pvAct = computeMenu->addAction(u"PV diagram…"_s);
        m_pvAct->setToolTip(u"A position-velocity diagram along a path, computed on the node and opened in place."_s);
        m_finderAct = computeMenu->addAction(u"Source finder…"_s);
        m_finderAct->setToolTip(u"Run a source finder on the node (through the Computing Broker when one is "
                                u"configured) and open its mask and catalogue."_s);
        computeMenu->setToolTipsVisible(true);
        m_computeBtn->setMenu(computeMenu);
        m_statusLabel = new QLabel(this);
        // An indeterminate bar, scoped to this panel, rather than an
        // application-wide wait cursor: the search is a network call, the rest
        // of the application stays usable during it, and a cursor that says
        // otherwise is a lie — one that used to outlive the request, because
        // nothing ever timed out.
        m_busyBar = new QProgressBar(this);
        m_busyBar->setRange(0, 0);           // indeterminate
        m_busyBar->setTextVisible(false);
        m_busyBar->setFixedSize(90, 6);
        m_busyBar->setVisible(false);
        bar->addWidget(m_searchBtn);
        bar->addSpacing(12);
        bar->addWidget(m_openBtn);
        bar->addWidget(m_openViaBtn);
        bar->addWidget(m_computeBtn);
        bar->addStretch(1);
        bar->addWidget(m_busyBar);
        bar->addWidget(m_statusLabel);
        root->addLayout(bar);
    }

    // ── Results and provenance, sharing the rest of the height ──────
    auto *split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);

    m_resultsTable = new QTableView(split);
    m_resultsTable->setModel(m_resultsModel);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->setTextElideMode(Qt::ElideMiddle);
    m_resultsTable->setWordWrap(false);
    m_resultsTable->setMinimumHeight(140);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->setAlternatingRowColors(true);
    {
        // Short columns fit their content; the target can be dragged wider;
        // the policy's reason takes what is left.
        auto *header = m_resultsTable->horizontalHeader();
        header->setMinimumSectionSize(40);
        for (int c = 0; c < SkavaResultsModel::ColCount; ++c)
            header->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(SkavaResultsModel::ColTarget, QHeaderView::Interactive);
        header->resizeSection(SkavaResultsModel::ColTarget, 200);
        header->setSectionResizeMode(SkavaResultsModel::ColReason, QHeaderView::Stretch);
    }

    // ── Provenance ───────────────────────────────────────────────────
    {
        auto *box = new QGroupBox(u"Provenance of the selected dataset"_s, split);
        auto *grid = makeGrid(box);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        // Long values (title, citation) wrap, so the label sits at the top of
        // its row rather than floating against the middle line.
        auto label = [box](const QString &text) {
            auto *l = new QLabel(text, box);
            l->setAlignment(Qt::AlignRight | Qt::AlignTop);
            return l;
        };
        m_provenanceTitle = new QLabel(u"-"_s, box);
        m_provenanceTitle->setWordWrap(true);
        m_provenanceTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_provenanceDoi = new QLabel(u"-"_s, box);
        m_provenanceDoi->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_provenanceDoi->setTextFormat(Qt::RichText);
        m_provenanceDoi->setOpenExternalLinks(true);
        m_provenancePid = new QLabel(u"-"_s, box);
        m_provenancePid->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_provenanceLicense = new QLabel(u"-"_s, box);
        m_provenanceLicense->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_provenanceCitation = new QLabel(u"-"_s, box);
        m_provenanceCitation->setWordWrap(true);
        m_provenanceCitation->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_copyCitationBtn = new QPushButton(u"Copy citation"_s, box);
        m_copyCitationBtn->setEnabled(false);
        // Two columns: the short identifiers side by side, the long texts
        // across the whole width.
        grid->addWidget(label(u"Title"_s), 0, 0);
        grid->addWidget(m_provenanceTitle, 0, 1, 1, 3);
        grid->addWidget(label(u"DOI"_s), 1, 0);
        grid->addWidget(m_provenanceDoi, 1, 1);
        grid->addWidget(label(u"PID"_s), 1, 2);
        grid->addWidget(m_provenancePid, 1, 3);
        grid->addWidget(label(u"License"_s), 2, 0);
        grid->addWidget(m_provenanceLicense, 2, 1, 1, 3);
        grid->addWidget(label(u"Citation"_s), 3, 0);
        grid->addWidget(m_provenanceCitation, 3, 1, 1, 2);
        grid->addWidget(m_copyCitationBtn, 3, 3, Qt::AlignRight | Qt::AlignTop);
        grid->setRowStretch(4, 1);
    }
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);
}

void SkavaSearchPanel::applySettingsToFields()
{
    if (!m_settings) return;
    m_urlEdit->setText(m_settings->getSkavaUrl());
    m_tokenEdit->setText(m_settings->getSkavaToken());
    const int index = m_protocolCombo->findData(m_settings->getSkavaProtocol());
    m_protocolCombo->setCurrentIndex(std::max(0, index));
    const bool vo = m_protocolCombo->currentData().toString() == "vo"_L1;
    m_tapEdit->setText(m_settings->getSkavaTapUrl());
    m_tapEdit->setEnabled(vo);
    m_datalinkEdit->setText(m_settings->getSkavaDatalinkUrl());
    m_datalinkEdit->setEnabled(vo);
    m_brokerEdit->setText(m_settings->getSkavaBrokerUrl());
}

std::shared_ptr<SkavaClient> SkavaSearchPanel::makeClient() const
{
    // A snapshot of the connection bar: the worker's client never changes
    // under it, whatever the user edits meanwhile.
    auto client = std::make_shared<SkavaClient>(m_urlEdit->text().trimmed(), m_tokenEdit->text().trimmed());
    client->setProtocol(SkavaClient::protocolFromString(m_protocolCombo->currentData().toString()));
    client->setTapUrl(m_tapEdit->text().trimmed());
    client->setDatalinkUrl(m_datalinkEdit->text().trimmed());
    return client;
}

// ── Sign-in ───────────────────────────────────────────────────────────────

OIDCAuthorizationCodeFlow *SkavaSearchPanel::ensureOidc()
{
    if (!m_settings)
        return nullptr;
    const QString authUrl = m_settings->getSkavaOidcAuthUrl();
    const QString tokenUrl = m_settings->getSkavaOidcTokenUrl();
    const QString clientId = m_settings->getSkavaOidcClientId();
    const QString scope = m_settings->getSkavaOidcScope();
    const QString key = authUrl + u'\n' + tokenUrl + u'\n' + clientId + u'\n' + scope;
    if (m_oidc && m_oidcKey == key)
        return m_oidc;
    if (m_oidc) {
        // Another IAM (or none): the old sign-in's token is no longer
        // refreshed, so nobody keeps using it.
        m_oidc->disconnect(this);
        m_oidc->deleteLater();
        m_oidc = nullptr;
        if (!m_settings->getSkavaSessionToken().isEmpty()) {
            m_settings->setSkavaSessionToken(QString());
            m_tokenEdit->setText(m_settings->getSkavaToken());
        }
    }
    m_oidcKey = key;
    if (authUrl.isEmpty() || tokenUrl.isEmpty() || clientId.isEmpty())
        return nullptr;
    // A public client: no secret, the IAM knows it by id (the testbed's
    // Keycloak, the SRCNet IAM with a registered public client).
    m_oidc = new OIDCAuthorizationCodeFlow(authUrl, tokenUrl, scope, clientId, QString(), this);
    m_oidc->setDisplayName(u"SKAVA"_s);
    // Every token the flow obtains, the first and each refresh, goes to the
    // consumers (the panel's client and, through Settings, the other flows).
    QObject::connect(m_oidc, &OIDCAuthorizationCodeFlow::accessTokenChanged, this, &SkavaSearchPanel::onSignedIn);
    QObject::connect(m_oidc, &OIDCAuthorizationCodeFlow::loggedOut, this, &SkavaSearchPanel::onSignedOut);
    return m_oidc;
}

void SkavaSearchPanel::updateSignInStatus()
{
    const bool configured = m_oidc != nullptr;
    const bool signedIn = configured && m_oidc->hasTokens();
    m_signInBtn->setText(signedIn ? u"Sign out"_s : u"Sign in…"_s);
    m_signInStatus->setText(signedIn ? u"signed in"_s : configured ? u"not signed in"_s : u"no IAM"_s);
    m_signInStatus->setToolTip(configured ? u"IAM: %1"_s.arg(m_settings ? m_settings->getSkavaOidcAuthUrl() : QString())
                                          : u"No IAM is configured (IAM…); a pasted token is used instead."_s);
}

void SkavaSearchPanel::onSignedIn()
{
    if (!m_oidc)
        return;
    // The token the IAM issued is what every SKAVA request sends from now
    // on: this panel's client, and the flows that read Settings (reopen, the
    // broker). It is short-lived and refreshed by the flow, so it lives in
    // memory only; the next start signs in again from the secure store.
    const QString token = m_oidc->getAccessToken();
    if (m_settings)
        m_settings->setSkavaSessionToken(token);
    m_tokenEdit->setText(token);   // the next request's client reads it from here
    updateSignInStatus();
    if (m_retryAfterSignIn) {
        const std::function<void()> retry = std::move(m_retryAfterSignIn);
        m_retryAfterSignIn = nullptr;
        if (m_busy)
            m_statusLabel->setText(u"Signed in; run the request again."_s);   // another one is out
        else
            retry();
    }
}

void SkavaSearchPanel::onSignedOut()
{
    m_retryAfterSignIn = nullptr;
    // The token field held the IAM's token: a sign-out takes it away. A
    // token the user pasted is in the settings and comes back from there.
    if (m_settings)
        m_settings->setSkavaSessionToken(QString());
    m_tokenEdit->setText(m_settings ? m_settings->getSkavaToken() : QString());
    updateSignInStatus();
}

void SkavaSearchPanel::signInOrOut()
{
    OIDCAuthorizationCodeFlow *oidc = ensureOidc();
    if (oidc && oidc->hasTokens()) {
        oidc->logout();
        return;
    }
    if (!oidc) {
        editSignInSettings();
        oidc = ensureOidc();
        if (!oidc)
            return;
    }
    oidc->grant();
}

void SkavaSearchPanel::editSignInSettings()
{
    if (!m_settings)
        return;
    QDialog dlg(this);
    dlg.setWindowTitle(u"Sign-in to SKAVA (OpenID Connect)"_s);
    auto *form = new QFormLayout(&dlg);
    auto *authEdit = new QLineEdit(m_settings->getSkavaOidcAuthUrl(), &dlg);
    authEdit->setPlaceholderText(u"https://iam.example/auth/realms/x/protocol/openid-connect/auth"_s);
    auto *tokenEdit = new QLineEdit(m_settings->getSkavaOidcTokenUrl(), &dlg);
    tokenEdit->setPlaceholderText(u"https://iam.example/auth/realms/x/protocol/openid-connect/token"_s);
    auto *clientEdit = new QLineEdit(m_settings->getSkavaOidcClientId(), &dlg);
    auto *scopeEdit = new QLineEdit(m_settings->getSkavaOidcScope(), &dlg);
    form->addRow(u"Authorization endpoint"_s, authEdit);
    form->addRow(u"Token endpoint"_s, tokenEdit);
    form->addRow(u"Client id"_s, clientEdit);
    form->addRow(u"Scope"_s, scopeEdit);
    auto *hint = new QLabel(u"A public client (no secret) registered with the IAM, with "
                            u"vlva://callback as its redirect URI. A service that answers 401 with "
                            u"an ivoa_bearer challenge fills the endpoints in by itself."_s, &dlg);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_settings->setSkavaOidcAuthUrl(authEdit->text());
    m_settings->setSkavaOidcTokenUrl(tokenEdit->text());
    m_settings->setSkavaOidcClientId(clientEdit->text());
    m_settings->setSkavaOidcScope(scopeEdit->text());
    ensureOidc();
    updateSignInStatus();
}

bool SkavaSearchPanel::offerSignIn(const SkavaAuthChallenge &challenge, std::function<void()> retry)
{
    OIDCAuthorizationCodeFlow *oidc = ensureOidc();
    if (!oidc && challenge.wellKnownUrl.isEmpty())
        return false;   // nothing to sign in with: the plain error is told
    const QString service = m_urlEdit->text().trimmed();
    const QString why = m_tokenEdit->text().trimmed().isEmpty()
            ? u"%1 requires a sign-in."_s.arg(service)
            : u"%1 did not accept the token."_s.arg(service);
    const auto answer = QMessageBox::question(
            this, u"Sign in to SKAVA"_s,
            u"%1\n\nSign in to %2 now? The request is repeated afterwards."_s.arg(
                    why, oidc ? m_settings->getSkavaOidcAuthUrl() : challenge.wellKnownUrl),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return true;    // declined: nothing more to say
    m_retryAfterSignIn = std::move(retry);
    if (oidc) {
        oidc->grant();
        return true;
    }
    // The challenge names the IAM: read its discovery document for the
    // endpoints, off the UI thread, then sign in. Cancel (the Search button
    // meanwhile) ends the read and drops the sign-in.
    const QString wellKnown = challenge.wellKnownUrl;
    setBusy(true, u"Reading the IAM's discovery document…"_s);
    const int generation = m_requestGeneration;
    const std::shared_ptr<SkavaClient> client = makeClient();
    m_activeClient = client;
    auto *watcher = new QFutureWatcher<SkavaClient::OidcEndpoints>(this);
    QObject::connect(watcher, &QFutureWatcher<SkavaClient::OidcEndpoints>::finished, this,
                     [this, watcher, generation]() {
        const SkavaClient::OidcEndpoints endpoints = watcher->result();
        watcher->deleteLater();
        setBusy(false);
        m_activeClient.reset();
        if (generation != m_requestGeneration) {
            m_retryAfterSignIn = nullptr;   // cancelled meanwhile
            return;
        }
        if (!endpoints.valid()) {
            m_retryAfterSignIn = nullptr;
            QMessageBox::warning(this, u"Sign in to SKAVA"_s,
                                 u"The IAM could not be configured from the service's challenge: %1\n\n"
                                 u"Set its endpoints with IAM…"_s.arg(endpoints.error));
            return;
        }
        m_settings->setSkavaOidcAuthUrl(endpoints.authorizationUrl);
        m_settings->setSkavaOidcTokenUrl(endpoints.tokenUrl);
        if (OIDCAuthorizationCodeFlow *flow = ensureOidc()) {
            updateSignInStatus();
            flow->grant();
        }
    });
    watcher->setFuture(QtConcurrent::run([client, wellKnown]() { return client->fetchOidcEndpoints(wellKnown); }));
    return true;
}

void SkavaSearchPanel::setBusy(bool busy, const QString &message)
{
    m_busy = busy;
    // The button stays enabled and becomes the way out of the request: a
    // network call can take tens of seconds against a slow or unreachable node,
    // and having to sit through it before being allowed to fix the URL and try
    // again is the whole complaint the deadline above only half answers.
    m_searchBtn->setEnabled(true);
    m_searchBtn->setText(busy ? u"Cancel"_s : u"Search"_s);
    m_searchBtn->setDefault(!busy);
    const bool hasSelection = !busy
                              && m_resultsTable->currentIndex().isValid()
                              && m_resultsTable->currentIndex().row() >= 0;
    m_openBtn->setEnabled(hasSelection);
    if (m_openViaBtn) m_openViaBtn->setEnabled(hasSelection && !m_openViaMenu->isEmpty());
    if (m_computeBtn) m_computeBtn->setEnabled(hasSelection);
    if (busy) {
        m_statusLabel->setText(message.isEmpty() ? u"Working…"_s : message);
    }
    if (m_busyBar)
        m_busyBar->setVisible(busy);
}

void SkavaSearchPanel::cancelRequest()
{
    if (!m_busy)
        return;
    // The worker is inside a blocking call; it polls the flag and unwinds on
    // its own. setBusy(false) is left to the watcher's finished handler, which
    // runs either way, so the panel cannot be re-enabled while the thread is
    // still out.
    if (m_activeClient)
        m_activeClient->requestCancel();
    ++m_requestGeneration;      // whatever ends later must not act
    m_searchBtn->setEnabled(false);
    m_statusLabel->setText(u"Cancelling…"_s);
}

void SkavaSearchPanel::runSearch()
{
    if (m_busy)
        return;   // one request at a time: see onResultSelectionChanged()
    if (m_urlEdit->text().trimmed().isEmpty()) {
        QMessageBox::information(this, u"SKAVA"_s,
                u"Set the SKAVA base URL before searching."_s);
        return;
    }

    SkavaSearchFilter f;
    auto readDouble = [](QLineEdit *e) -> double {
        const QString t = e->text().trimmed();
        if (t.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
        bool ok = false;
        const double v = t.toDouble(&ok);
        return ok ? v : std::numeric_limits<double>::quiet_NaN();
    };
    f.posRa     = readDouble(m_raEdit);
    f.posDec    = readDouble(m_decEdit);
    f.posRadius = readDouble(m_radiusEdit);
    // Convert frequency/wavelength range to metres for SKAVA.
    const double bandMinUser = readDouble(m_freqMinEdit);
    const double bandMaxUser = readDouble(m_freqMaxEdit);
    const QString unit = m_freqUnit->currentText();
    if (std::isfinite(bandMinUser) && std::isfinite(bandMaxUser)) {
        if (unit == "m"_L1) {
            f.bandMinMetres = std::min(bandMinUser, bandMaxUser);
            f.bandMaxMetres = std::max(bandMinUser, bandMaxUser);
        } else {
            // user gave frequencies → convert each to wavelength; the
            // *higher* frequency maps to the *smaller* wavelength.
            const double hzMin = unitToHz(bandMinUser, unit);
            const double hzMax = unitToHz(bandMaxUser, unit);
            const double lambdaLo = frequencyHzToWavelengthM(std::max(hzMin, hzMax));
            const double lambdaHi = frequencyHzToWavelengthM(std::min(hzMin, hzMax));
            f.bandMinMetres = lambdaLo;
            f.bandMaxMetres = lambdaHi;
        }
    }
    f.timeMinMjd = readDouble(m_timeMinEdit);
    f.timeMaxMjd = readDouble(m_timeMaxEdit);
    f.collection      = m_collectionEdit->text().trimmed();
    f.dataproductType = m_dptypeCombo->currentText().trimmed();
    f.obsId           = m_obsIdEdit->text().trimmed();
    f.limit  = m_limitSpin->value();
    f.offset = m_offsetSpin->value();

    setBusy(true, u"Searching…"_s);
    const std::shared_ptr<SkavaClient> client = makeClient();
    m_activeClient = client;
    m_submittedGeneration = m_requestGeneration;
    m_searchWatcher->setFuture(QtConcurrent::run([client, f]() {
        return client->searchDiscovery(f);
    }));
}

void SkavaSearchPanel::onResultSelectionChanged()
{
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) {
        m_openBtn->setEnabled(false);
        m_openViaMenu->clear();
        if (m_openViaBtn) m_openViaBtn->setEnabled(false);
        if (m_computeBtn) m_computeBtn->setEnabled(false);
        m_provenanceTitle->setText(u"-"_s);
        m_provenanceDoi->setText(u"-"_s);
        m_provenancePid->setText(u"-"_s);
        m_provenanceLicense->setText(u"-"_s);
        m_provenanceCitation->setText(u"-"_s);
        m_copyCitationBtn->setEnabled(false);
        return;
    }
    const auto &meta = all[row].metadata;
    // Not while a request is out: Open and SODA share the client and the one
    // DataLink watcher with the search, so launching a second one would have
    // the first watcher's setBusy(false) hand the panel back while a worker is
    // still using the client — and one cancel flag would serve both.
    m_openBtn->setEnabled(!m_busy);
    if (m_computeBtn) m_computeBtn->setEnabled(!m_busy);
    // The products of a cube: a 2-D image has no moments, PV or line finder.
    const bool cube = meta.dataproductType.trimmed().compare(u"cube"_s, Qt::CaseInsensitive) == 0;
    m_momentAct->setEnabled(cube);
    m_pvAct->setEnabled(cube);
    // "Open via…": one entry per replica, SKAVA's order and reasons.
    m_openViaMenu->clear();
    for (const auto &rep : all[row].replicas) {
        const QString code = rep.code;
        QString text = rep.code == all[row].bestNode ? u"%1 (ranked first)"_s.arg(code) : code;
        if (!rep.name.isEmpty() && rep.name != rep.code)
            text += u" — %1"_s.arg(rep.name);
        QAction *act = m_openViaMenu->addAction(text, this, [this, code]() { openSelected(code); });
        act->setToolTip(rep.rankReason);
        act->setEnabled(rep.isEnabled && rep.isAvailable);
    }
    m_openViaMenu->setToolTipsVisible(true);
    if (m_openViaBtn) m_openViaBtn->setEnabled(!m_busy && !m_openViaMenu->isEmpty());
    m_provenanceTitle->setText(
            meta.obsTitle.isEmpty() ? u"-"_s : meta.obsTitle);
    if (!meta.doi.isEmpty()) {
        const QString href = meta.doi.startsWith("http"_L1)
                ? meta.doi : (u"https://doi.org/"_s + meta.doi);
        m_provenanceDoi->setText(
                QStringLiteral("<a href='%1'>%2</a>").arg(href, meta.doi));
    } else {
        m_provenanceDoi->setText(u"-"_s);
    }
    m_provenancePid->setText(meta.pid.isEmpty() ? u"-"_s : meta.pid);
    m_provenanceLicense->setText(meta.license.isEmpty() ? u"-"_s : meta.license);
    m_provenanceCitation->setText(meta.citation.isEmpty() ? u"-"_s : meta.citation);
    m_copyCitationBtn->setEnabled(!meta.citation.isEmpty());
}

void SkavaSearchPanel::openSelected(const QString &preferredNode)
{
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) return;
    resolveRow(all[row], QString(), preferredNode);
}

void SkavaSearchPanel::requestOperation(const QString &kind)
{
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) return;
    resolveRow(all[row], kind, QString());
}

void SkavaSearchPanel::resolveRow(const SkavaDiscoveryResult &row, const QString &operation,
                                  const QString &preferredNode)
{
    if (m_busy)
        return;   // one request at a time: see onResultSelectionChanged()
    if (row.metadata.obsId.isEmpty())
        return;
    // The shared DataLink resolution; the watcher reads these to know what
    // the user asked for once it completes.
    m_pendingOperation = operation;
    m_pendingNode = preferredNode;
    m_pendingRow = row;
    setBusy(true, !operation.isEmpty() ? u"Resolving access for the operation…"_s
            : preferredNode.isEmpty()  ? u"Resolving access URL…"_s
                                       : u"Resolving access via %1…"_s.arg(preferredNode));
    const std::shared_ptr<SkavaClient> client = makeClient();
    m_activeClient = client;
    m_submittedGeneration = m_requestGeneration;
    m_datalinkWatcher->setFuture(QtConcurrent::run([client, row]() {
        return client->resolveAccess(row);
    }));
}

void SkavaSearchPanel::copyToClipboard()
{
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) return;
    QApplication::clipboard()->setText(all[row].metadata.citation);
    m_statusLabel->setText(u"Citation copied to clipboard."_s);
}

#include "SkavaSearchPanel.moc"
