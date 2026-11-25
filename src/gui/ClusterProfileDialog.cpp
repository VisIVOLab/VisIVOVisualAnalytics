#include "ClusterProfileDialog.h"

#include "app/ClusterConnection.h"

#include <algorithm>

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

ClusterProfileDialog::ClusterProfileDialog(const QString &settingsPath, QWidget *parent)
    : QDialog(parent), m_settingsPath(settingsPath)
{
    setWindowTitle(u"Connect to a cluster"_s);
    resize(560, 0);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
            u"Submits the VisIVO backend as a SLURM job on the cluster and opens a new window "
            u"on it, through an SSH tunnel. This window stays on its current backend. Closing "
            u"the new window closes the tunnel and cancels the job.\n\nNeeds SSH access to the "
            u"frontend that asks for nothing when it connects (a key, or the site's SSH "
            u"certificate: sign in to it first). Nothing has to be installed on the cluster: "
            u"this client sends the launcher and pulls the backend image of its own version. "
            u"Choose your site, then fill in your login, project account and data folder."_s,
            this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout();
    m_name = new QComboBox(this);
    m_name->setEditable(true);
    m_name->setToolTip(u"The name shown in the status bar; type a new one for a new profile."_s);
    form->addRow(u"Name"_s, m_name);
    m_site = new QComboBox(this);
    m_site->addItem(ClusterProfile::presetLabel({}), QString());
    for (const QString &site : ClusterProfile::presetSites())
        m_site->addItem(ClusterProfile::presetLabel(site), site);
    m_site->setToolTip(u"A known site fills in its login node, partition, GPU, resources and "
                       u"time; your login, account and data folder stay yours."_s);
    form->addRow(u"Site"_s, m_site);
    const auto line = [this, form](const QString &label, const QString &placeholder,
                                   const QString &tip = {}) {
        auto *e = new QLineEdit(this);
        e->setPlaceholderText(placeholder);
        if (!tip.isEmpty())
            e->setToolTip(tip);
        form->addRow(label, e);
        return e;
    };
    m_frontend = line(u"Frontend"_s, u"pleiadi-gpu.oact.inaf.it"_s, u"The SLURM login node, reached with ssh."_s);
    m_user = line(u"User"_s, u"your login on the frontend (empty: the local user)"_s);
    m_repoDir = line(u"VisIVO clone"_s, u"empty: this client sends the launcher"_s,
                     u"Only for developers: a VisIVO clone on the frontend, relative to the home "
                     u"or absolute, whose own launcher is run instead of the client's."_s);
    m_partition = line(u"Partition"_s, u"gpu"_s);
    m_account = line(u"Account"_s, u"SLURM project, if the cluster requires one"_s,
                     u"Charged for the job (sbatch --account); required on Leonardo and MareNostrum 5."_s);
    m_qos = line(u"QOS"_s, u"optional (sbatch --qos)"_s);
    m_gres = line(u"GPUs (gres)"_s, u"gpu:1"_s);
    m_cpus = new QSpinBox(this);
    m_cpus->setRange(1, 256);
    m_cpus->setValue(16);
    form->addRow(u"CPUs"_s, m_cpus);
    m_mem = line(u"Memory"_s, u"96G"_s);
    m_time = line(u"Time limit"_s, u"08:00:00 (empty: the launcher's default)"_s);
    m_queueWait = new QSpinBox(this);
    m_queueWait->setRange(0, 24 * 60);
    m_queueWait->setSuffix(u" min"_s);
    m_queueWait->setSpecialValueText(u"launcher default (5 min)"_s);
    m_queueWait->setToolTip(u"How long to wait for the cluster to give the job a node. Cancel "
                            u"in the connection window stops earlier."_s);
    form->addRow(u"Wait in queue"_s, m_queueWait);
    m_image = line(u"Image to pull"_s, u"empty: ghcr.io/visivolab/visivo-backend:<this client's version>"_s,
                   u"A docker://... reference, pulled once on the frontend. Empty: the published image "
                   u"of this client's version (with the -egl tag when a GPU is requested). Set it "
                   u"for a client that is not a release, or to pin another version."_s);
    m_sif = line(u"Backend image file"_s, u"/scratch/<user>/backend-egl.sif"_s,
                 u"An image file already on the cluster; used instead of pulling."_s);
    m_dataRoot = line(u"Data root"_s, u"/scratch/<user>/data"_s, u"The folder the backend may open data from."_s);
    m_workspace = line(u"Workspace"_s, u"/scratch/<user>/visivo-ws"_s,
                       u"Exports, pyramids and logs of the backend; on a large disk, not the home."_s);
    m_nodeCode = line(u"SRC node code"_s, u"optional, for SKAVA"_s);
    m_extra = line(u"More options"_s, u"--backend-src /scratch/... (passed to the launcher)"_s);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(this);
    auto *connectBtn = buttons->addButton(u"Connect"_s, QDialogButtonBox::AcceptRole);
    auto *saveBtn = buttons->addButton(u"Save"_s, QDialogButtonBox::ApplyRole);
    auto *deleteBtn = buttons->addButton(u"Delete"_s, QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    QObject::connect(m_name, &QComboBox::textActivated, this, [this](const QString &name) {
        QSettings s(m_settingsPath, QSettings::IniFormat);
        const ClusterProfile p = ClusterProfiles::find(s, name);
        if (p.isValid())
            showProfile(p);
    });
    QObject::connect(m_site, &QComboBox::activated, this, [this](int index) {
        // A site fills in what it defines, over what the fields hold now.
        ClusterProfile p = current();
        p.applyPreset(m_site->itemData(index).toString());
        showProfile(p);
    });
    QObject::connect(saveBtn, &QPushButton::clicked, this, [this]() { saveCurrent(); });
    QObject::connect(deleteBtn, &QPushButton::clicked, this, [this]() {
        const QString name = m_name->currentText().trimmed();
        QSettings s(m_settingsPath, QSettings::IniFormat);
        QList<ClusterProfile> all = ClusterProfiles::load(s);
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [&name](const ClusterProfile &p) { return p.name == name; }),
                  all.end());
        ClusterProfiles::save(s, all);
        reloadNames(QString());
    });
    QObject::connect(connectBtn, &QPushButton::clicked, this, [this]() {
        if (const QString problem = current().launcherProblem(QCoreApplication::applicationVersion());
            !problem.isEmpty()) {
            QMessageBox::warning(this, u"Connect to a cluster"_s, problem);
            return;
        }
        if (!saveCurrent())
            return;
        QString error;
        if (!openWindowOn(current().name, {}, &error)) {
            QMessageBox::critical(this, u"Connect to a cluster"_s, error);
            return;
        }
        accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    reloadNames(QString());
}

void ClusterProfileDialog::reloadNames(const QString &select)
{
    QSettings s(m_settingsPath, QSettings::IniFormat);
    const QList<ClusterProfile> all = ClusterProfiles::load(s);
    m_name->clear();
    for (const auto &p : all)
        m_name->addItem(p.name);
    const int idx = select.isEmpty() ? 0 : m_name->findText(select);
    if (!all.isEmpty() && idx >= 0) {
        m_name->setCurrentIndex(idx);
        showProfile(all.at(idx));
    } else {
        showProfile(ClusterProfile{});
    }
}

void ClusterProfileDialog::showProfile(const ClusterProfile &p)
{
    m_name->setEditText(p.name);
    m_frontend->setText(p.frontend);
    m_user->setText(p.user);
    m_repoDir->setText(p.repoDir);
    // What the profile stores, not what its host suggests: saving an older profile
    // must not add a --site that its clone may not know.
    m_site->setCurrentIndex(std::max(0, m_site->findData(p.site)));
    m_partition->setText(p.partition);
    m_account->setText(p.account);
    m_qos->setText(p.qos);
    m_gres->setText(p.gres);
    m_cpus->setValue(p.cpus > 0 ? p.cpus : 16);
    m_mem->setText(p.mem);
    m_time->setText(p.time);
    m_sif->setText(p.sif);
    m_image->setText(p.image);
    m_queueWait->setValue(p.queueWaitMin);
    m_dataRoot->setText(p.dataRoot);
    m_workspace->setText(p.workspace);
    m_nodeCode->setText(p.nodeCode);
    m_extra->setText(ClusterProfile::joinCommand(p.extraArgs));
}

ClusterProfile ClusterProfileDialog::current() const
{
    ClusterProfile p;
    p.name = m_name->currentText().trimmed();
    p.frontend = m_frontend->text().trimmed();
    p.user = m_user->text().trimmed();
    p.repoDir = m_repoDir->text().trimmed();
    p.site = m_site->currentData().toString();
    p.partition = m_partition->text().trimmed();
    p.account = m_account->text().trimmed();
    p.qos = m_qos->text().trimmed();
    p.gres = m_gres->text().trimmed();
    p.cpus = m_cpus->value();
    p.mem = m_mem->text().trimmed();
    p.time = m_time->text().trimmed();
    p.sif = m_sif->text().trimmed();
    p.image = m_image->text().trimmed();
    p.queueWaitMin = m_queueWait->value();
    p.dataRoot = m_dataRoot->text().trimmed();
    p.workspace = m_workspace->text().trimmed();
    p.nodeCode = m_nodeCode->text().trimmed();
    p.extraArgs = QProcess::splitCommand(m_extra->text().trimmed());
    return p;
}

bool ClusterProfileDialog::saveCurrent()
{
    const ClusterProfile p = current();
    if (!p.isValid()) {
        QMessageBox::warning(this, u"Connect to a cluster"_s,
                             u"A profile needs a name and a frontend."_s);
        return false;
    }
    if (const QString problem = ClusterConnection::profileProblem(p); !problem.isEmpty()) {
        QMessageBox::warning(this, u"Connect to a cluster"_s, problem);
        return false;
    }
    QSettings s(m_settingsPath, QSettings::IniFormat);
    ClusterProfiles::upsert(s, p);
    s.sync();
    reloadNames(p.name);
    return true;
}

bool ClusterProfileDialog::openWindowOn(const QString &name, const QStringList &extraArgs,
                                        QString *error)
{
    // The new window chooses its backend by the profile alone: a remote backend
    // this window was started on (VISIVO_BACKEND_*) must not reach it.
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(QStringList{ u"--connect-cluster"_s, name } + extraArgs);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &var : { u"VISIVO_BACKEND_URL"_s, u"VISIVO_BACKEND_TOKEN"_s,
                                u"VISIVO_BACKEND_TOKEN_FILE"_s, u"VISIVO_BACKEND_NAME"_s })
        env.remove(var);
    process.setProcessEnvironment(env);
    if (!process.startDetached()) {
        *error = u"Could not start a new VisIVO window (%1)."_s.arg(
                QCoreApplication::applicationFilePath());
        return false;
    }
    return true;
}
