#ifndef HiPSCatalogueDialog_h
#define HiPSCatalogueDialog_h

#include "app/BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

// ---------------------------------------------------------------------------
// HiPSCatalogueDialog — what to overlay on the sky.
//
// The viewer could only overlay a CSV already sitting on the backend, which
// meant finding and downloading one first. This also runs a VO cone search
// over the field on screen and overlays what comes back, which is the way you
// actually ask "what is in this field".
// ---------------------------------------------------------------------------

class HiPSCatalogueDialog : public QDialog
{
    Q_OBJECT

public:
    HiPSCatalogueDialog(const QString &backendUrl, const QString &backendToken,
                        double raDeg, double decDeg, double fovDeg,
                        QWidget *parent = nullptr);

    // Path of the catalogue to overlay, and the columns holding its position
    // (empty when the backend could not name them and the defaults apply).
    QString cataloguePath() const { return m_cataloguePath; }
    QString raField() const { return m_raField; }
    QString decField() const { return m_decField; }
    QString describeSource() const { return m_description; }

private slots:
    void onModeChanged();
    void onBrowseFile();
    void onAccept();
    void onSearchFinished();
    void onServiceChanged(int index);
    void onServicesLoaded();

private:
    // One place decides what is enabled: the mode, whether the service list
    // has arrived, and whether a search is in flight. Three call sites each
    // enabling their own widgets is how a search ended up with a live service
    // combo driving a dead URL field.
    void updateControls();
    // The display name of a service URL, or the URL when it is not one of the
    // known ones — the combo's current text is not the same thing.
    QString serviceNameFor(const QString &url) const;

    QString m_backendUrl;
    QString m_backendToken;
    double  m_raDeg{ 0.0 };
    double  m_decDeg{ 0.0 };

    QString m_cataloguePath;
    QString m_raField;
    QString m_decField;
    QString m_description;

    QPointer<QRadioButton>    m_coneRadio;
    QPointer<QRadioButton>    m_fileRadio;
    QPointer<QComboBox>       m_serviceCombo;
    QPointer<QLineEdit>       m_serviceUrlEdit;
    QPointer<QDoubleSpinBox>  m_radiusSpin;
    QPointer<QLabel>          m_centreLabel;
    QPointer<QLineEdit>       m_pathEdit;
    QPointer<QPushButton>     m_browseButton;
    QPointer<QLabel>          m_statusLabel;
    QPointer<QDialogButtonBox> m_buttons;

    QList<BackendConeSearchService> m_services;
    QString m_searchedService;
    bool    m_servicesLoading{ true };
    bool    m_searching{ false };
    QFutureWatcher<QList<BackendConeSearchService>> m_servicesWatcher;
    QFutureWatcher<BackendConeSearchResult> m_searchWatcher;
};

#endif // HiPSCatalogueDialog_h
