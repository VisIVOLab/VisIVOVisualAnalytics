#include "UpdateNotifier.h"

#include "Settings.h"
#include "UpdateCheck.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

using namespace Qt::StringLiterals;
namespace upd = visivo::update;

namespace {
constexpr int kStartupDelayMs = 5000;    // let the window settle before asking
constexpr int kRequestTimeoutMs = 10000;
}   // namespace

UpdateNotifier::UpdateNotifier(Settings *settings, QWidget *window)
    : QObject(window), m_settings(settings), m_window(window),
      m_network(new QNetworkAccessManager(this))
{
}

void UpdateNotifier::scheduleStartupCheck()
{
    if (!m_settings || !m_settings->getCheckForUpdates())
        return;
    if (!upd::isCheckDue(m_settings->getLastUpdateCheck(), QDateTime::currentDateTimeUtc()))
        return;
    QTimer::singleShot(kStartupDelayMs, this, [this]() {
        // Re-asked at fire time: the user may have switched the check off, or
        // run a manual check, in the first seconds.
        if (m_settings && m_settings->getCheckForUpdates()
            && upd::isCheckDue(m_settings->getLastUpdateCheck(), QDateTime::currentDateTimeUtc()))
            this->check(/*manual=*/false);
    });
}

void UpdateNotifier::check(bool manual)
{
    if (m_pending) {
        // One request at a time; a manual click during the automatic check
        // makes that check's answer visible instead of starting a second one.
        m_pendingManual = m_pendingManual || manual;
        return;
    }
    QNetworkRequest request(upd::latestReleaseApiUrl());
    // GitHub rejects API requests without a User-Agent.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      u"VisIVO-Visual-Analytics/%1"_s.arg(QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(kRequestTimeoutMs);
    m_pendingManual = manual;
    m_pending = m_network->get(request);
    QNetworkReply *reply = m_pending;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const bool wasManual = m_pendingManual;
        m_pending = nullptr;
        m_pendingManual = false;
        // Scheduled BEFORE onFinished: its message boxes run a nested event
        // loop, during which this notifier (and the manager that owns the
        // reply) can be destroyed; nothing may touch the reply afterwards.
        reply->deleteLater();
        this->onFinished(reply, wasManual);
    });
}

void UpdateNotifier::onFinished(QNetworkReply *reply, bool manual)
{
    const QString current = QCoreApplication::applicationVersion();
    if (reply->error() != QNetworkReply::NoError) {
        // A failed check is not recorded, so the next start tries again.
        if (manual && m_window) {
            QMessageBox::warning(m_window, u"Check for Updates"_s,
                                 u"Could not reach the release service.\n\n%1"_s
                                         .arg(reply->errorString()));
        }
        return;
    }
    const upd::LatestRelease latest = upd::parseLatestRelease(reply->readAll());
    if (!latest.valid) {
        if (manual && m_window) {
            QMessageBox::warning(m_window, u"Check for Updates"_s,
                                 u"Could not read the latest release: %1."_s.arg(latest.error));
        }
        return;
    }
    if (m_settings)
        m_settings->setLastUpdateCheck(QDateTime::currentDateTimeUtc());

    if (upd::isNewer(latest.version, current)) {
        if (!manual && m_settings && m_settings->getSkippedVersion() == latest.version)
            return;
        this->showNewVersion(latest.version, current, latest.pageUrl, manual);
        return;
    }
    if (!manual || !m_window)
        return;
    QString text;
    if (!upd::isComparableVersion(current))
        text = u"This build (%1) cannot be compared with the latest release, %2."_s
                       .arg(current, latest.version);
    else if (upd::isNewer(current, latest.version))
        text = u"You are running %1, newer than the latest release, %2."_s
                       .arg(current, latest.version);
    else
        text = u"VisIVO Visual Analytics %1 is the latest release."_s.arg(current);
    QMessageBox::information(m_window, u"Check for Updates"_s, text);
}

void UpdateNotifier::showNewVersion(const QString &latest, const QString &current,
                                    const QUrl &page, bool manual)
{
    if (!m_window)
        return;
    auto *box = new QMessageBox(QMessageBox::Information, u"Update Available"_s,
                                u"VisIVO Visual Analytics %1 is available."_s.arg(latest),
                                QMessageBox::NoButton, m_window);
    box->setInformativeText(u"You are running %1. The release page has the installers "
                            u"for every platform and the list of changes."_s.arg(current));
    box->setAttribute(Qt::WA_DeleteOnClose);
    // Non-modal: the notice must not stand between the user and their work.
    box->setWindowModality(Qt::NonModal);
    QPushButton *download = box->addButton(u"Download…"_s, QMessageBox::AcceptRole);
    QPushButton *skip = box->addButton(u"Skip This Version"_s, QMessageBox::DestructiveRole);
    box->addButton(u"Later"_s, QMessageBox::RejectRole);
    box->setDefaultButton(download);
    // A manual check reports even a skipped release, and says so.
    if (manual && m_settings && m_settings->getSkippedVersion() == latest)
        box->setInformativeText(box->informativeText()
                                + u"\n\nYou chose to skip this version earlier."_s);
    QPointer<Settings> settings = m_settings;
    QObject::connect(box, &QMessageBox::buttonClicked, box,
                     [download, skip, page, latest, settings](QAbstractButton *clicked) {
                         if (clicked == download)
                             QDesktopServices::openUrl(page);
                         else if (clicked == skip && settings)
                             settings->setSkippedVersion(latest);
                     });
    box->show();
}
