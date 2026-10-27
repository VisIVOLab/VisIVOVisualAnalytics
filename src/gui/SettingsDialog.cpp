#include "SettingsDialog.h"
#include "ui_SettingsDialog.h"

#include "AuthWrapper.h"
#include "BackendsSettingsWidget.h"
#include "Settings.h"

#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QScrollArea>
#include <QTabBar>
#include <QTabWidget>

#include <limits>

using namespace Qt::StringLiterals;

SettingsDialog::SettingsDialog(Settings *settings, AuthWrapper *auth, QWidget *parent)
    : QDialog(parent), ui(new Ui::SettingsDialog), settings(settings), auth(auth)
{
    ui->setupUi(this);

    // Reload just in case the file has been modified by another program
    this->settings->reload();

    // UI
    ui->comboTheme->setCurrentIndex(static_cast<int>(this->settings->getColorScheme()));
    ui->comboPlotTheme->setCurrentIndex(this->settings->getPlotTheme());

    // Python
    ui->linePythonPath->setText(this->settings->getPythonLocation());
    ui->btnPython->setIcon(QIcon(u":/icons/FILE_OPEN.png"_s));
    QObject::connect(ui->btnPython, &QToolButton::clicked, this,
                     &SettingsDialog::selectPythonLocation);

    // Panoramic View
    ui->linePanoramicView->setText(this->settings->getPanoramicView());
    ui->btnPanoramicView->setIcon(QIcon(u":/icons/FILE_OPEN.png"_s));
    QObject::connect(ui->btnPanoramicView, &QToolButton::clicked, this,
                     &SettingsDialog::selectPanoramicView);

    // Visualization group
    ui->lineMaxGlyphs->setText(QString::number(this->settings->getMaxGlyphs()));
    ui->lineMaxGlyphs->setValidator(
            new QIntValidator(0, std::numeric_limits<int>::max(), ui->lineMaxGlyphs));

    // Backend group
    ui->lineBackendUrl->setText(this->settings->getBackendUrl());
    ui->lineBackendToken->setText(this->settings->getBackendToken());
    QObject::connect(ui->btnBackendTokenShow, &QToolButton::toggled, this,
                     [this](bool checked) {
                         ui->lineBackendToken->setEchoMode(
                                 checked ? QLineEdit::EchoMode::Normal
                                         : QLineEdit::EchoMode::Password);
                     });
    QObject::connect(ui->btnBackendTokenCopy, &QToolButton::clicked, this, [this]() {
        const QString token = ui->lineBackendToken->text().trimmed();
        if (!token.isEmpty()) {
            QGuiApplication::clipboard()->setText(token);
        }
    });

    // VLKB group
    ui->lineVLKBUrl->setText(this->settings->getVLKBUrl());
    this->setVLKBAuthStatus();
    ui->checkVLKBSearch->setChecked(this->settings->getSearchOnImportFlag());
    ui->checkUpdates->setChecked(this->settings->getCheckForUpdates());
    QObject::connect(ui->btnVLKBAuth, &QPushButton::clicked, this,
                     &SettingsDialog::VLKBAuthTriggered);

    // Remote backends (distributed-backend pattern — one node per SRC).
    // Inject the widget into the dialog's vertical layout just above the
    // buttonBox so it appears at the bottom alongside the other groups.
    this->backendsWidget = new BackendsSettingsWidget(this->settings, this);

    // Scientific-copilot LLM provider section (persisted here; the Copilot panel
    // re-applies it to the backend on Settings::updated). Both this and the
    // remote-backends widget are placed by buildTabs() below.
    QWidget *copilotSection = this->buildCopilotSection();

    // Restore defaults
    QObject::connect(ui->buttonBox, &QDialogButtonBox::clicked, this,
                     [this](QAbstractButton *button) {
                         if (ui->buttonBox->buttonRole(button) == QDialogButtonBox::ResetRole) {
                             this->restoreDefaults();
                         }
                     });

    // Update labels when user's authentication status changes
    QObject::connect(this->auth, &AuthWrapper::granted, this, &SettingsDialog::updateAuthStatus);
    QObject::connect(this->auth, &AuthWrapper::loggedOut, this, &SettingsDialog::updateAuthStatus);

    // Signal-to-signal connection to notify other classes they need to reload
    // their settings
    QObject::connect(this, &SettingsDialog::accepted, this->settings, &Settings::updated);

    // Group the sections into tabs rather than stacking all eight of them in one
    // column. Stacked, the dialog is taller than most screens and the only way
    // to reach the copilot or the remote backends is a long scroll past settings
    // nobody was looking for; the button box also drifts off-screen. Each tab
    // keeps its own scroll area, so a tab that still overflows (a long list of
    // remote backends) stays reachable, and the button box is pinned outside
    // them all.
    this->buildTabs(copilotSection);
}

// Move the sections built by the .ui file (plus the two built in code) into a
// QTabWidget. The widgets are reparented, never recreated, so every pointer the
// rest of this class holds — ui->lineVLKBUrl, m_copilotKey, … — stays valid.
void SettingsDialog::buildTabs(QWidget *copilotSection)
{
    auto *outer = qobject_cast<QVBoxLayout *>(this->layout());
    if (!outer)
        return;

    // Take the sections out of the dialog's layout before anything is added
    // back, so nothing is left behind if the .ui gains a section later: whatever
    // is not claimed by a tab below goes to the first one.
    outer->removeWidget(ui->buttonBox);
    QList<QWidget *> loose;
    while (outer->count() > 0) {
        QLayoutItem *item = outer->takeAt(0);
        if (QWidget *w = item->widget())
            loose.append(w);
        delete item;   // the wrapper, not the widget
    }

    auto *tabs = new QTabWidget(this);

    // One tab = a scroll area over a column of group boxes.
    auto addTab = [&](const QString &title, const QList<QWidget *> &sections) {
        auto *page = new QWidget;
        auto *col = new QVBoxLayout(page);
        col->setContentsMargins(4, 8, 4, 4);
        col->setSpacing(8);
        for (QWidget *w : sections) {
            if (!w)
                continue;
            loose.removeOne(w);
            col->addWidget(w);
        }
        col->addStretch(1);   // sections keep their height, they don't spread
        auto *scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        tabs->addTab(scroll, title);
        return page;
    };

    QWidget *first = addTab(tr("General"), { ui->groupUI, ui->groupPython,
                                             ui->groupVisualization });
    addTab(tr("Data Access"), { ui->groupVLKB, ui->groupPanoramicView });
    addTab(tr("Backends"), { ui->groupBackend, this->backendsWidget });
    addTab(tr("Copilot"), { copilotSection });

    // Anything the .ui grew that no tab claimed (and the spacer it ends with).
    if (auto *col = qobject_cast<QVBoxLayout *>(first->layout())) {
        for (QWidget *w : std::as_const(loose))
            col->insertWidget(col->count() - 1, w);
    }

    outer->addWidget(tabs);
    outer->addWidget(ui->buttonBox);

    // Open large enough to show a whole tab. sizeHint() cannot be used here: a
    // QScrollArea reports a small hint of its own regardless of its contents, so
    // asking the dialog collapsed it to a few hundred pixels and every tab
    // opened already scrolled. Measure the widest page instead, and cap against
    // the screen.
    int w = 0, h = 0;
    for (int i = 0; i < tabs->count(); ++i) {
        if (auto *sa = qobject_cast<QScrollArea *>(tabs->widget(i))) {
            if (QWidget *page = sa->widget()) {
                w = qMax(w, page->sizeHint().width());
                h = qMax(h, page->sizeHint().height());
            }
        }
    }
    // Room for the tab bar, the button box, the frame and a scrollbar.
    w += 60;
    h += tabs->tabBar()->sizeHint().height() + ui->buttonBox->sizeHint().height() + 60;
    // Floor first, then the screen cap — the other way round a small display
    // would be overruled by the floor and the button box would sit off-screen.
    w = qMax(w, 620);
    h = qMax(h, 480);
    if (QScreen *scr = this->screen()) {
        const QRect avail = scr->availableGeometry();
        w = qMin(w, static_cast<int>(avail.width() * 0.9));
        h = qMin(h, static_cast<int>(avail.height() * 0.9));
    }
    this->resize(w, h);
}

SettingsDialog::~SettingsDialog()
{
    delete ui;
}

void SettingsDialog::accept()
{
    this->settings->setColorScheme(static_cast<Qt::ColorScheme>(ui->comboTheme->currentIndex()));
    this->settings->setPlotTheme(ui->comboPlotTheme->currentIndex());
    this->settings->setPanoramicView(ui->linePanoramicView->text());
    this->settings->setMaxGlyphs(ui->lineMaxGlyphs->text().toInt());
    this->settings->setVLKBUrl(ui->lineVLKBUrl->text());
    this->settings->setSearchOnImportFlag(ui->checkVLKBSearch->isChecked());
    this->settings->setCheckForUpdates(ui->checkUpdates->isChecked());
    this->settings->setBackendUrl(ui->lineBackendUrl->text().trimmed());
    this->settings->setBackendToken(ui->lineBackendToken->text().trimmed());
    if (this->backendsWidget) {
        this->backendsWidget->saveTo(this->settings);
    }
    if (this->m_copilotProvider) {
        this->settings->setCopilotProvider(this->m_copilotProvider->currentIndex());
        this->settings->setCopilotKey(this->m_copilotKey->text());
        this->settings->setCopilotUrl(this->m_copilotUrl->text().trimmed());
        this->settings->setCopilotModel(this->m_copilotModel->text().trimmed());
    }
    QDialog::accept();   // emits accepted → Settings::updated (the Copilot panel re-applies)
}

QWidget *SettingsDialog::buildCopilotSection()
{
    auto *group = new QGroupBox(tr("Scientific Copilot"), this);
    auto *form = new QFormLayout(group);

    m_copilotProvider = new QComboBox(group);
    m_copilotProvider->addItem(tr("Claude (Anthropic)"));               // 0
    m_copilotProvider->addItem(tr("ChatGPT (OpenAI)"));                 // 1
    m_copilotProvider->addItem(tr("Your server (OpenAI-compatible)"));  // 2
    m_copilotProvider->addItem(tr("None"));                            // 3

    m_copilotKey = new QLineEdit(group);
    m_copilotKey->setEchoMode(QLineEdit::Password);
    m_copilotKey->setPlaceholderText(tr("API key"));
    m_copilotUrl = new QLineEdit(group);
    m_copilotUrl->setPlaceholderText(u"http://your-server:8000/v1"_s);
    m_copilotModel = new QLineEdit(group);
    m_copilotModel->setPlaceholderText(tr("model name (optional)"));

    form->addRow(tr("Provider"), m_copilotProvider);
    form->addRow(tr("API key"), m_copilotKey);
    form->addRow(tr("Server URL"), m_copilotUrl);
    form->addRow(tr("Model"), m_copilotModel);

    // Show only the relevant field(s) per provider (cloud → key; own server → URL
    // + key; none → nothing), matching the Copilot panel's ⚙.
    auto refresh = [this, form]() {
        const int i = m_copilotProvider->currentIndex();
        m_copilotKey->setVisible(i == 0 || i == 1 || i == 2);
        m_copilotUrl->setVisible(i == 2);
        m_copilotModel->setVisible(i != 3);
        if (auto *l = form->labelForField(m_copilotKey)) l->setVisible(m_copilotKey->isVisible());
        if (auto *l = form->labelForField(m_copilotUrl)) l->setVisible(m_copilotUrl->isVisible());
        if (auto *l = form->labelForField(m_copilotModel)) l->setVisible(m_copilotModel->isVisible());
    };
    QObject::connect(m_copilotProvider, &QComboBox::currentIndexChanged, this,
                     [refresh](int) { refresh(); });

    // Pre-fill from the persisted config.
    m_copilotProvider->setCurrentIndex(this->settings->getCopilotProvider());
    m_copilotKey->setText(this->settings->getCopilotKey());
    m_copilotUrl->setText(this->settings->getCopilotUrl());
    m_copilotModel->setText(this->settings->getCopilotModel());
    refresh();
    return group;
}

void SettingsDialog::restoreDefaults()
{
    this->settings->resetDefaults();
    QDialog::accept();
}

void SettingsDialog::selectPythonLocation()
{
    const QString filepath =
            QFileDialog::getOpenFileName(this, u"Select Python interpreter"_s, QDir::homePath(),
                                         u"python3 executable (python3 python)"_s);
    if (!filepath.isEmpty()) {
        ui->linePythonPath->setText(filepath);
    }
}

void SettingsDialog::selectPanoramicView()
{
    const QString filepath = QFileDialog::getOpenFileName(
            this, u"Select Panoramic View"_s, QDir::homePath(), u"Panoramic View index (*.html)"_s);
    if (!filepath.isEmpty()) {
        ui->linePanoramicView->setText(QUrl::fromUserInput(filepath).toString());
    }
}

void SettingsDialog::updateAuthStatus(AuthService service)
{
    if (service == AuthService::VLKB) {
        this->setVLKBAuthStatus();
    }
}

void SettingsDialog::VLKBAuthTriggered()
{
    if (this->auth->hasTokens(AuthService::VLKB)) {
        this->auth->logout(AuthService::VLKB);
    } else {
        this->auth->grant(AuthService::VLKB);
    }
}

void SettingsDialog::setVLKBAuthStatus()
{
    // A build made without the VLKB client configuration cannot sign in: say so
    // rather than offer a button that silently does nothing.
    if (!this->auth->isConfigured(AuthService::VLKB)) {
        ui->labelVLKBAuth->setText(u"Not available in this build"_s);
        ui->btnVLKBAuth->setText(u"Sign in"_s);
        ui->btnVLKBAuth->setEnabled(false);
        const QString why = u"This build was made without the VLKB sign-in configuration "
                            u"(VISIVO_VLKB_* at build time). Public VLKB services still work; "
                            u"authenticated ones need a build that includes it."_s;
        ui->btnVLKBAuth->setToolTip(why);
        ui->labelVLKBAuth->setToolTip(why);
        return;
    }
    ui->btnVLKBAuth->setEnabled(true);
    ui->btnVLKBAuth->setToolTip(QString());
    ui->labelVLKBAuth->setToolTip(QString());
    if (this->auth->hasTokens(AuthService::VLKB)) {
        ui->labelVLKBAuth->setText(u"Authenticated"_s);
        ui->btnVLKBAuth->setText(u"Sign out"_s);
    } else {
        ui->labelVLKBAuth->setText(u"Not authenticated"_s);
        ui->btnVLKBAuth->setText(u"Sign in"_s);
    }
}
