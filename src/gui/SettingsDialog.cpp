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
    QObject::connect(ui->btnVLKBAuth, &QPushButton::clicked, this,
                     &SettingsDialog::VLKBAuthTriggered);

    // Remote backends (distributed-backend pattern — one node per SRC).
    // Inject the widget into the dialog's vertical layout just above the
    // buttonBox so it appears at the bottom alongside the other groups.
    this->backendsWidget = new BackendsSettingsWidget(this->settings, this);
    if (auto *box = qobject_cast<QBoxLayout *>(this->layout())) {
        const int buttonBoxIndex = box->indexOf(ui->buttonBox);
        if (buttonBoxIndex >= 0) {
            box->insertWidget(buttonBoxIndex, this->backendsWidget);
        } else {
            box->addWidget(this->backendsWidget);
        }
    }

    // Scientific-copilot LLM provider section (persisted here; the Copilot panel
    // re-applies it to the backend on Settings::updated).
    if (auto *box = qobject_cast<QBoxLayout *>(this->layout())) {
        QWidget *copilot = this->buildCopilotSection();
        const int buttonBoxIndex = box->indexOf(ui->buttonBox);
        if (buttonBoxIndex >= 0)
            box->insertWidget(buttonBoxIndex, copilot);
        else
            box->addWidget(copilot);
    }

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

    // Make the dialog scrollable: the stacked sections can exceed the window (and
    // the screen) height, leaving the lower sections / buttons unreachable. Move
    // every section into a QScrollArea, keeping the button box pinned below it.
    if (auto *outer = qobject_cast<QVBoxLayout *>(this->layout())) {
        auto *content = new QWidget;
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        outer->removeWidget(ui->buttonBox);   // keep the buttons outside the scroll area
        while (outer->count() > 0) {
            QLayoutItem *item = outer->takeAt(0);
            if (QWidget *w = item->widget()) {
                contentLayout->addWidget(w);  // reparents w to `content`
                delete item;                  // free the now-empty wrapper (not the widget)
            } else {
                contentLayout->addItem(item); // layouts / spacers transfer as-is
            }
        }
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        outer->addWidget(scroll);
        outer->addWidget(ui->buttonBox);
    }
    // Open at a height that fits the screen; the scroll area handles any overflow.
    if (QScreen *scr = this->screen()) {
        const int fitH = qMin(this->sizeHint().height(),
                              static_cast<int>(scr->availableGeometry().height() * 0.9));
        this->resize(this->width(), fitH);
    }
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
    if (this->auth->hasTokens(AuthService::VLKB)) {
        ui->labelVLKBAuth->setText(u"Authenticated"_s);
        ui->btnVLKBAuth->setText(u"Sign out"_s);
    } else {
        ui->labelVLKBAuth->setText(u"Not authenticated"_s);
        ui->btnVLKBAuth->setText(u"Sign in"_s);
    }
}
