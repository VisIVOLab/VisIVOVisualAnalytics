#ifndef ClusterProfileDialog_h
#define ClusterProfileDialog_h

#include "app/ClusterProfile.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

/**
 * Data Hub ▸ Connect to a cluster…: the saved cluster profiles, editable, and
 * Connect, which saves the profile and opens a new VisIVO window on that
 * cluster (`--connect-cluster NAME`). This window stays on its own backend.
 */
class ClusterProfileDialog : public QDialog
{
    Q_OBJECT

public:
    /// @p settingsPath: the client's Settings.ini, where the profiles are kept.
    explicit ClusterProfileDialog(const QString &settingsPath, QWidget *parent = nullptr);

    /// Starts a new VisIVO process on the cluster profile @p name.
    static bool openWindowOn(const QString &name, QString *error);

private:
    void showProfile(const ClusterProfile &p);
    ClusterProfile current() const;
    void reloadNames(const QString &select);
    bool saveCurrent();

    QString m_settingsPath;
    QComboBox *m_name;
    QLineEdit *m_frontend, *m_user, *m_repoDir, *m_partition, *m_gres, *m_mem, *m_time, *m_sif,
            *m_dataRoot, *m_workspace, *m_nodeCode, *m_extra;
    QSpinBox *m_cpus;
};

#endif
