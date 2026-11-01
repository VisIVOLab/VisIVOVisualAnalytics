#include "ClusterConnectDialog.h"

#include "app/ClusterConnection.h"

#include <QCoreApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

ClusterConnectDialog::ClusterConnectDialog(ClusterConnection *connection, QWidget *parent)
    : QDialog(parent), m_connection(connection)
{
    const QString name = connection->profile().name;
    setWindowTitle(u"VisIVO Visual Analytics — %1"_s.arg(name));
    resize(640, 360);

    auto *layout = new QVBoxLayout(this);
    m_status = new QLabel(u"Connecting to %1 (%2)…"_s.arg(name, connection->profile().sshTarget()), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    layout->addWidget(m_log, 1);
    m_cancel = new QPushButton(u"Cancel"_s, this);
    layout->addWidget(m_cancel, 0, Qt::AlignRight);

    QObject::connect(connection, &ClusterConnection::message, this,
                     [this](const QString &text) { m_log->appendPlainText(text); });
    QObject::connect(connection, &ClusterConnection::connected, this, [this]() {
        m_finished = true;
        accept();
    });
    QObject::connect(connection, &ClusterConnection::failed, this, [this, name](const QString &reason) {
        m_finished = true;
        m_status->setText(u"Could not connect to %1.\n\n%2"_s.arg(name, reason));
        m_log->appendPlainText(reason);
        // Whatever was submitted is cancelled now, not when the window closes.
        m_cancel->setEnabled(false);
        m_log->appendPlainText(u"Closing the tunnel and cancelling the job…"_s);
        QCoreApplication::processEvents();   // shown before the wait for scancel
        m_connection->stop();
        m_cancel->setText(u"Close"_s);
        m_cancel->setEnabled(true);
    });
    QObject::connect(m_cancel, &QPushButton::clicked, this, &ClusterConnectDialog::reject);
}

void ClusterConnectDialog::cancelConnection()
{
    if (m_finished)
        return;
    m_finished = true;
    m_cancel->setEnabled(false);
    m_status->setText(u"Cancelling: closing the tunnel and the job…"_s);
    QCoreApplication::processEvents();   // shown before the wait for scancel
    m_connection->stop();
}

void ClusterConnectDialog::reject()
{
    cancelConnection();
    QDialog::reject();
}

int ClusterConnectDialog::exec()
{
    QTimer::singleShot(0, m_connection, &ClusterConnection::start);
    return QDialog::exec();
}
