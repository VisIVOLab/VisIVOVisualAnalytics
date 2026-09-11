#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "AboutDialog.h"
#include "AuthWrapper.h"
#include "app/BackendClient.h"
#include "app/BackendClientFactory.h"
#include "app/SampClient.h"
#include "app/SampMessageHandler.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "BaselineDialog.h"
#include "CommandPalette.h"
#include "DataHubWidget.h"
#include "DiagnosticsWindow.h"
#include "LinewidthDialog.h"
#include "RemoteFileBrowserDialog.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "theme/VisivoTheme.h"
#include "SourceFindDialog.h"
#include "StackDialog.h"
#include "WebViewProcess.h"
#include "vtkWindowCatalogue3D.h"
#include "vtkWindowCube.h"
#include "vtkWindowImage.h"

#include <QLabel>
#include <QStatusBar>
#include "HiPSWindow.h"
#include "vtkWindowVbt.h"
#include "vtkWindowVbtVolume.h"
#include "VLKBInventoryTree.h"
#include "SkavaSearchPanel.h"
#include "../skava/SkavaClient.h"
#include "../Settings.h"
#include "vlkb/VlkbQueryService.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QWebChannel>
#include <QPixmap>
#include <QStyle>
#include <QPainter>
#include <QUrl>
#include <QtConcurrent>

#include <cmath>

#include "app/DiagnosticsManager.h"

#include <vtkImageData.h>

using namespace Qt::StringLiterals;

// ── MainWindow: Science (extracted from MainWindow.cpp) ──

void MainWindow::runPublicationFigure()
{
    if (m_sciDatasetId.isEmpty()) {
        QMessageBox::information(this, u"Publication Figure"_s, u"Open a dataset first."_s);
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(u"Publication Figure"_s);
    auto *form = new QFormLayout(&dlg);
    auto *stretch = new QComboBox(&dlg);
    stretch->addItems(QStringList{ u"linear"_s, u"log"_s, u"sqrt"_s, u"asinh"_s, u"power"_s });
    auto *cmap = new QComboBox(&dlg);
    cmap->addItems(QStringList{ u"inferno"_s, u"viridis"_s, u"magma"_s, u"gray"_s, u"hot"_s, u"cubehelix"_s });
    auto *title = new QLineEdit(&dlg);
    auto *grid = new QCheckBox(u"WCS sky grid"_s, &dlg); grid->setChecked(true);
    auto *cbar = new QCheckBox(u"Colorbar (BUNIT)"_s, &dlg); cbar->setChecked(true);
    form->addRow(u"Stretch"_s, stretch);
    form->addRow(u"Colormap"_s, cmap);
    form->addRow(u"Title"_s, title);
    form->addRow(QString(), grid);
    form->addRow(QString(), cbar);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Render"_s);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString url = m_sciBackendUrl, token = m_sciToken, ds = m_sciDatasetId;
    const QString sessionId = m_sciSessionId;
    const QString s = stretch->currentText(), cm = cmap->currentText(), t = title->text();
    const bool g = grid->isChecked(), c = cbar->isChecked();
    auto *watcher = new QFutureWatcher<BackendPublicationFigureResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendPublicationFigureResult>::finished, this,
                     [this, watcher]() {
        QApplication::restoreOverrideCursor();
        watcher->deleteLater();
        const auto r = watcher->result();
        if (!r.valid) {
            QMessageBox::critical(this, u"Publication Figure failed"_s,
                    r.error.isEmpty() ? u"No detail."_s : r.error);
            return;
        }
        const auto choice = QMessageBox::information(
                this, u"Publication Figure"_s,
                u"Figure saved on the backend:\n%1\n\nOpen it now? (works when the "
                u"backend shares this machine's filesystem)"_s.arg(r.outputPath),
                QMessageBox::Open | QMessageBox::Close, QMessageBox::Open);
        if (choice == QMessageBox::Open)
            QDesktopServices::openUrl(QUrl::fromLocalFile(r.outputPath));
    });
    QApplication::setOverrideCursor(Qt::WaitCursor);
    watcher->setFuture(QtConcurrent::run([url, token, sessionId, ds, s, cm, t, g, c]() {
        BackendClient bc(url, token);
        bc.setSessionId(sessionId);
        return bc.publicationFigure(ds, s, t, g, c, cm, 200);
    }));
}


void MainWindow::runImageQuality()
{
    if (m_sciDatasetId.isEmpty()) {
        QMessageBox::information(this, u"Image Quality"_s, u"Open a dataset first."_s);
        return;
    }
    const QString url = m_sciBackendUrl, token = m_sciToken, ds = m_sciDatasetId;
    const QString sessionId = m_sciSessionId;
    auto *watcher = new QFutureWatcher<BackendImageQualityResult>(this);
    QObject::connect(watcher, &QFutureWatcher<BackendImageQualityResult>::finished, this,
                     [this, watcher]() {
        QApplication::restoreOverrideCursor();
        watcher->deleteLater();
        const auto r = watcher->result();
        if (!r.valid) {
            QMessageBox::critical(this, u"Image Quality failed"_s,
                    r.error.isEmpty() ? u"No detail."_s : r.error);
            return;
        }
        const QStringList order = {
            u"total_pixels"_s, u"blanked_fraction"_s, u"zero_fraction"_s,
            u"negative_fraction"_s, u"min"_s, u"max"_s, u"mean"_s, u"median"_s,
            u"robust_std"_s, u"dynamic_range"_s, u"striping_metric"_s,
            u"negative_bowl"_s };
        QString text;
        for (const QString &k : order) {
            if (!r.metrics.contains(k)) continue;
            text += u"%1 : %2\n"_s.arg(k, -18).arg(r.metrics.value(k).toString());
        }
        const QStringList flags = r.metrics.value(u"flags"_s).toStringList();
        text += u"\nflags : %1"_s.arg(flags.isEmpty() ? u"(none, looks clean)"_s
                                                       : flags.join(u", "_s));
        QDialog box(this);
        box.setWindowTitle(u"Image Quality / Artifacts"_s);
        auto *lay = new QVBoxLayout(&box);
        auto *edit = new QPlainTextEdit(&box);
        edit->setReadOnly(true);
        edit->setPlainText(text);
        edit->setMinimumSize(380, 280);
        lay->addWidget(edit);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &box);
        QObject::connect(bb, &QDialogButtonBox::rejected, &box, &QDialog::reject);
        QObject::connect(bb, &QDialogButtonBox::accepted, &box, &QDialog::accept);
        lay->addWidget(bb);
        box.exec();
    });
    QApplication::setOverrideCursor(Qt::WaitCursor);
    watcher->setFuture(QtConcurrent::run([url, token, sessionId, ds]() {
        BackendClient bc(url, token);
        bc.setSessionId(sessionId);
        return bc.imageQuality(ds);
    }));
}


void MainWindow::runPixelMath()
{
    if (m_sciSessionId.isEmpty()) {
        QMessageBox::information(this, u"Pixel Math"_s,
                u"Open a dataset first to establish a backend session, then open the "
                u"map(s) you want to combine."_s);
        return;
    }
    const QString url = m_sciBackendUrl, token = m_sciToken, sessionId = m_sciSessionId;
    const QString currentId = m_sciDatasetId;
    auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>(this);
    QPointer<MainWindow> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished, this,
                     [self, watcher, url, token, sessionId, currentId]() {
        const auto listing = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        if (!listing.valid) {
            QMessageBox::warning(self, u"Pixel Math"_s,
                    u"Could not list session datasets: %1"_s.arg(listing.error));
            return;
        }
        QDialog dlg(self);
        dlg.setWindowTitle(u"Pixel Math / Spectral Index"_s);
        auto *form = new QFormLayout(&dlg);
        auto *comboA = new QComboBox(&dlg);
        auto *comboB = new QComboBox(&dlg);
        for (const auto &d : listing.datasets) {
            const QString label = QFileInfo(d.path).fileName().isEmpty()
                    ? d.datasetId : QFileInfo(d.path).fileName();
            comboA->addItem(label, d.datasetId);
            comboB->addItem(label, d.datasetId);
        }
        const int curIdx = comboA->findData(currentId);
        if (curIdx >= 0) comboA->setCurrentIndex(curIdx);
        if (comboB->count() > 1) comboB->setCurrentIndex(comboA->currentIndex() == 0 ? 1 : 0);
        auto *op = new QComboBox(&dlg);
        op->addItems(QStringList{ u"ratio"_s, u"divide"_s, u"subtract"_s, u"add"_s,
                                   u"multiply"_s, u"spectral_index"_s, u"log10"_s,
                                   u"abs"_s, u"square"_s });
        auto *freqA = new QLineEdit(&dlg); freqA->setPlaceholderText(u"freq A (Hz)"_s);
        freqA->setValidator(new QDoubleValidator(&dlg));
        auto *freqB = new QLineEdit(&dlg); freqB->setPlaceholderText(u"freq B (Hz)"_s);
        freqB->setValidator(new QDoubleValidator(&dlg));
        form->addRow(u"Map A"_s, comboA);
        form->addRow(u"Map B"_s, comboB);
        form->addRow(u"Operation"_s, op);
        form->addRow(u"Freq A (spectral index)"_s, freqA);
        form->addRow(u"Freq B (spectral index)"_s, freqB);
        auto syncEnabled = [&]() {
            const QString o = op->currentText();
            const bool unary = (o == u"log10"_s || o == u"abs"_s || o == u"square"_s);
            const bool si = (o == u"spectral_index"_s);
            comboB->setEnabled(!unary);
            freqA->setEnabled(si);
            freqB->setEnabled(si);
        };
        QObject::connect(op, &QComboBox::currentTextChanged, &dlg, [syncEnabled](const QString &){ syncEnabled(); });
        syncEnabled();
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(u"Compute"_s);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;

        const QString idA = comboA->currentData().toString();
        const QString o = op->currentText();
        const bool unary = (o == u"log10"_s || o == u"abs"_s || o == u"square"_s);
        const QString idB = unary ? QString() : comboB->currentData().toString();
        const double fA = freqA->text().toDouble();
        const double fB = freqB->text().toDouble();

        auto *w2 = new QFutureWatcher<BackendProductFitsResult>(self);
        QObject::connect(w2, &QFutureWatcher<BackendProductFitsResult>::finished, self,
                         [self, w2, url, token, o]() {
            QApplication::restoreOverrideCursor();
            w2->deleteLater();
            if (!self) return;
            const auto r = w2->result();
            if (!r.valid) {
                QMessageBox::critical(self, u"Pixel Math failed"_s,
                        r.error.isEmpty() ? u"No detail."_s : r.error);
                return;
            }
            self->openBackendProductFits(url, token, r.outputPath, u"Pixel math: %1"_s.arg(o));
        });
        QApplication::setOverrideCursor(Qt::WaitCursor);
        w2->setFuture(QtConcurrent::run([url, token, sessionId, idA, o, idB, fA, fB]() {
            BackendClient bc(url, token);
            bc.setSessionId(sessionId);  // dataset_ids live in this session
            return bc.mapMath(idA, o, idB, fA, fB);
        }));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
        BackendClient c(url, token);
        c.setSessionId(sessionId);
        return c.listSessionDatasets(sessionId);
    }));
}


void MainWindow::runMosaic()
{
    if (m_sciSessionId.isEmpty()) {
        QMessageBox::information(this, u"Mosaic"_s,
                u"Open a dataset first to establish a backend session, then open the "
                u"images you want to mosaic."_s);
        return;
    }
    const QString url = m_sciBackendUrl, token = m_sciToken, sessionId = m_sciSessionId;
    auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>(this);
    QPointer<MainWindow> self(this);
    QObject::connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished, this,
                     [self, watcher, url, token, sessionId]() {
        const auto listing = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        if (!listing.valid) {
            QMessageBox::warning(self, u"Mosaic"_s,
                    u"Could not list session datasets: %1"_s.arg(listing.error));
            return;
        }
        QDialog dlg(self);
        dlg.setWindowTitle(u"Mosaic (noise-weighted)"_s);
        auto *lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel(u"Select two or more images to mosaic:"_s, &dlg));
        auto *list = new QListWidget(&dlg);
        list->setSelectionMode(QAbstractItemView::MultiSelection);
        for (const auto &d : listing.datasets) {
            const QString label = QFileInfo(d.path).fileName().isEmpty()
                    ? d.datasetId : QFileInfo(d.path).fileName();
            auto *item = new QListWidgetItem(label, list);
            item->setData(Qt::UserRole, d.datasetId);
        }
        lay->addWidget(list);
        auto *weight = new QComboBox(&dlg);
        weight->addItem(u"Noise-weighted (1/σ²)"_s, u"noise"_s);
        weight->addItem(u"Uniform"_s, u"uniform"_s);
        auto *wrow = new QFormLayout;
        wrow->addRow(u"Weighting"_s, weight);
        lay->addLayout(wrow);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(u"Build mosaic"_s);
        lay->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;

        QStringList ids;
        for (QListWidgetItem *it : list->selectedItems())
            ids << it->data(Qt::UserRole).toString();
        if (ids.size() < 2) {
            QMessageBox::information(self, u"Mosaic"_s, u"Select at least two images."_s);
            return;
        }
        const QString wmode = weight->currentData().toString();

        auto *w2 = new QFutureWatcher<BackendProductFitsResult>(self);
        QObject::connect(w2, &QFutureWatcher<BackendProductFitsResult>::finished, self,
                         [self, w2, url, token]() {
            QApplication::restoreOverrideCursor();
            w2->deleteLater();
            if (!self) return;
            const auto r = w2->result();
            if (!r.valid) {
                QMessageBox::critical(self, u"Mosaic failed"_s,
                        r.error.isEmpty() ? u"No detail."_s : r.error);
                return;
            }
            self->openBackendProductFits(url, token, r.outputPath, u"Mosaic"_s);
        });
        QApplication::setOverrideCursor(Qt::WaitCursor);
        w2->setFuture(QtConcurrent::run([url, token, sessionId, ids, wmode]() {
            BackendClient bc(url, token);
            bc.setSessionId(sessionId);  // dataset_ids live in this session
            return bc.mosaic(ids, wmode);
        }));
    });
    watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
        BackendClient c(url, token);
        c.setSessionId(sessionId);
        return c.listSessionDatasets(sessionId);
    }));
}
