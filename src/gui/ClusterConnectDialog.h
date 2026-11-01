#ifndef ClusterConnectDialog_h
#define ClusterConnectDialog_h

#include <QDialog>

class ClusterConnection;
class QLabel;
class QPlainTextEdit;
class QPushButton;

/**
 * The start-up screen of a window opened on a cluster (`--connect-cluster`):
 * what the connection is doing, with Cancel. Accepted once the backend answers
 * through the tunnel; rejected on failure or Cancel (the job is then cancelled).
 */
class ClusterConnectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ClusterConnectDialog(ClusterConnection *connection, QWidget *parent = nullptr);

    int exec() override;
    /// Cancel, the title bar's close button and Esc: the job is cancelled first.
    void reject() override;

private:
    void cancelConnection();

    ClusterConnection *m_connection;
    QLabel *m_status;
    QPlainTextEdit *m_log;
    QPushButton *m_cancel;
    bool m_finished = false;
};

#endif
