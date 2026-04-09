#include "VisivoTheme.h"

#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSvgRenderer>
#include <QPainter>
#include <QWidget>

using namespace Qt::StringLiterals;

namespace VisivoTheme {

// ── Font loading ──────────────────────────────────────────────────────────────

bool loadBundledFonts()
{
    bool ok = true;

    const struct { const char *path; const char *label; } kFonts[] = {
        { ":/fonts/Lato-Light.ttf",   "Lato Light"   },
        { ":/fonts/Lato-Regular.ttf", "Lato Regular" },
        { ":/fonts/Lato-Bold.ttf",    "Lato Bold"    },
        { ":/fonts/Lato-Black.ttf",   "Lato Black"   },
    };

    for (const auto &f : kFonts) {
        const int id = QFontDatabase::addApplicationFont(QLatin1StringView(f.path));
        if (id < 0) {
            qWarning("[VisivoTheme] failed to load bundled font: %s", f.label);
            ok = false;
        }
    }

    return ok;
}

// ── Component factories ───────────────────────────────────────────────────────

QLabel *makeSectionLabel(const QString &text, QWidget *parent)
{
    auto *lbl = new QLabel(text.toUpper(), parent);
    lbl->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  color: %1;"
                    "  font-family: %2;"
                    "  font-size: 10px;"
                    "  font-weight: 700;"
                    "  letter-spacing: 0.18em;"
                    "  background-color: transparent;"
                    "}")
                    .arg(QLatin1StringView(kOnSurfaceVariant),
                         QLatin1StringView(kBrandFamily)));
    return lbl;
}

QFrame *makeCard(QWidget *parent, bool elevated)
{
    auto *card = new QFrame(parent);
    const QLatin1StringView bg(elevated ? kSurfaceContainerHighest : kSurfaceContainerHigh);
    card->setStyleSheet(
            QStringLiteral(
                    "QFrame {"
                    "  background-color: %1;"
                    "  border-radius: 8px;"
                    "  border: 1px solid rgba(4, 138, 191, 0.14);"
                    "}")
                    .arg(bg));
    return card;
}

QLabel *makeBadge(const QString &text, BadgeKind kind, QWidget *parent)
{
    auto *lbl = new QLabel(text, parent);
    QLatin1StringView bg, fg;
    switch (kind) {
    case BadgeKind::Success:
        bg = QLatin1StringView(kTertiaryContainer);
        fg = QLatin1StringView(kOnTertiaryFixed);
        break;
    case BadgeKind::Error:
        bg = QLatin1StringView(kErrorContainer);
        fg = QLatin1StringView(kOnErrorContainer);
        break;
    case BadgeKind::Warning:
        bg = QLatin1StringView(kWarningContainer);
        fg = QLatin1StringView(kOnWarningContainer);
        break;
    default:
        bg = QLatin1StringView(kSurfaceContainerHighest);
        fg = QLatin1StringView(kOnSurfaceVariant);
        break;
    }
    lbl->setStyleSheet(
            QStringLiteral(
                    "QLabel {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  font-family: %3;"
                    "  font-size: 10px;"
                    "  font-weight: 700;"
                    "  letter-spacing: 0.12em;"
                    "  padding: 3px 10px;"
                    "  border-radius: 10px;"
                    "  border: none;"
                    "}")
                    .arg(bg, fg, QLatin1StringView(kBrandFamily)));
    return lbl;
}

QPushButton *makePrimaryButton(const QString &text, QWidget *parent)
{
    auto *btn = new QPushButton(text, parent);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
            QStringLiteral(
                    "QPushButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  font-family: %3;"
                    "  font-size: 13px;"
                    "  font-weight: 700;"
                    "  padding: 10px 24px;"
                    "  border-radius: 6px;"
                    "  border: none;"
                    "}"
                    "QPushButton:hover {"
                    "  background-color: %4;"
                    "}"
                    "QPushButton:pressed {"
                    "  background-color: %5;"
                    "}"
                    "QPushButton:disabled {"
                    "  background-color: rgba(4, 138, 191, 0.25);"
                    "  color: rgba(255,255,255, 0.45);"
                    "}")
                    .arg(QLatin1StringView(kPrimary),
                         QLatin1StringView(kOnPrimary),
                         QLatin1StringView(kBrandFamily),
                         QLatin1StringView(kPrimaryDim),
                         QLatin1StringView(kPrimaryFixed)));
    return btn;
}

QPushButton *makeSecondaryButton(const QString &text, QWidget *parent)
{
    auto *btn = new QPushButton(text, parent);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
            QStringLiteral(
                    "QPushButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  font-family: %3;"
                    "  font-size: 12px;"
                    "  font-weight: 600;"
                    "  padding: 8px 18px;"
                    "  border-radius: 6px;"
                    "  border: 1px solid rgba(4, 138, 191, 0.38);"
                    "}"
                    "QPushButton:hover {"
                    "  background-color: %4;"
                    "  border-color: %5;"
                    "  color: %2;"
                    "}"
                    "QPushButton:pressed {"
                    "  background-color: %6;"
                    "}")
                    .arg(QLatin1StringView(kSurfaceContainerHigh),
                         QLatin1StringView(kPrimary),
                         QLatin1StringView(kBrandFamily),
                         QLatin1StringView(kSurfaceContainerHighest),
                         QLatin1StringView(kPrimary),
                         QLatin1StringView(kSurfaceBright)));
    return btn;
}

// ── Global QSS ────────────────────────────────────────────────────────────────

QString globalStyleSheet()
{
    // Lato is the VisIVO brand typeface. It is bundled and registered in
    // loadBundledFonts() before this stylesheet is applied, so the font-family
    // reference here will resolve to the bundled copy on all platforms.
    return QStringLiteral(
            // ── Base ──────────────────────────────────────────────────────
            "QWidget {"
            "  background-color: #091E30;"
            "  color: #E6F4F9;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  selection-background-color: rgba(4, 138, 191, 0.30);"
            "  selection-color: #E6F4F9;"
            "}"

            // ── Main window / dialog shells ────────────────────────────────
            "QMainWindow, QDialog {"
            "  background-color: #091E30;"
            "}"
            "QMainWindow::separator {"
            "  width: 1px; height: 1px;"
            "  background-color: rgba(4, 138, 191, 0.20);"
            "}"

            // ── Status bar ─────────────────────────────────────────────────
            "QStatusBar {"
            "  background-color: #0E2940;"
            "  color: #8AB8CC;"
            "  font-size: 11px;"
            "  border-top: 1px solid rgba(4, 138, 191, 0.25);"
            "}"

            // ── Menu bar ───────────────────────────────────────────────────
            // Uses the VisIVO corporate navy — consistent with brand imagery.
            "QMenuBar {"
            "  background-color: #0E2940;"
            "  color: #E6F4F9;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  padding: 2px 4px;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.22);"
            "  spacing: 2px;"
            "}"
            "QMenuBar::item {"
            "  padding: 6px 12px;"
            "  border-radius: 4px;"
            "  background-color: transparent;"
            "}"
            "QMenuBar::item:selected {"
            "  background-color: #12334F;"
            "  color: #048ABF;"
            "}"
            "QMenuBar::item:pressed {"
            "  background-color: #163D5E;"
            "  color: #048ABF;"
            "}"

            // ── Drop-down menus ────────────────────────────────────────────
            "QMenu {"
            "  background-color: #12334F;"
            "  color: #E6F4F9;"
            "  border: 1px solid rgba(4, 138, 191, 0.28);"
            "  border-radius: 6px;"
            "  padding: 4px;"
            "}"
            "QMenu::item {"
            "  padding: 8px 28px 8px 16px;"
            "  border-radius: 4px;"
            "}"
            "QMenu::item:selected {"
            "  background-color: #163D5E;"
            "  color: #048ABF;"
            "}"
            "QMenu::item:disabled {"
            "  color: rgba(138, 184, 204, 0.4);"
            "}"
            "QMenu::separator {"
            "  height: 1px;"
            "  background-color: rgba(4, 138, 191, 0.20);"
            "  margin: 4px 8px;"
            "}"

            // ── Toolbar ────────────────────────────────────────────────────
            "QToolBar {"
            "  background-color: #0E2940;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.22);"
            "  spacing: 4px;"
            "  padding: 3px 6px;"
            "}"
            "QToolBar::separator {"
            "  width: 1px;"
            "  background-color: rgba(4, 138, 191, 0.22);"
            "  margin: 4px 2px;"
            "}"
            "QToolButton {"
            "  background-color: transparent;"
            "  color: #E6F4F9;"
            "  padding: 5px 8px;"
            "  border-radius: 4px;"
            "  border: none;"
            "}"
            "QToolButton:hover {"
            "  background-color: #12334F;"
            "  color: #048ABF;"
            "}"
            "QToolButton:pressed, QToolButton:checked {"
            "  background-color: #163D5E;"
            "  color: #048ABF;"
            "}"

            // ── Tab widget ─────────────────────────────────────────────────
            "QTabWidget::pane {"
            "  border: none;"
            "  background-color: #091E30;"
            "}"
            "QTabBar {"
            "  background-color: #0E2940;"
            "  qproperty-drawBase: 0;"
            "}"
            "QTabBar::tab {"
            "  background-color: transparent;"
            "  color: #8AB8CC;"
            "  padding: 9px 22px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  border: none;"
            "  border-bottom: 2px solid transparent;"
            "  margin-right: 2px;"
            "}"
            "QTabBar::tab:selected {"
            "  color: #048ABF;"
            "  border-bottom: 2px solid #048ABF;"
            "}"
            "QTabBar::tab:hover:!selected {"
            "  color: #E6F4F9;"
            "  background-color: rgba(18, 51, 79, 0.6);"
            "}"

            // ── Dock widget ────────────────────────────────────────────────
            // Title uses Lato uppercase tracked — consistent with brand section labels.
            "QDockWidget {"
            "  background-color: #0E2940;"
            "  color: #E6F4F9;"
            "  border: none;"
            "  titlebar-close-icon: url(none);"
            "  titlebar-normal-icon: url(none);"
            "}"
            "QDockWidget::title {"
            "  background-color: #12334F;"
            "  color: #8AB8CC;"
            "  padding: 8px 14px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.18);"
            "}"
            "QDockWidget::close-button, QDockWidget::float-button {"
            "  background-color: transparent;"
            "  border: none;"
            "  border-radius: 3px;"
            "  padding: 2px;"
            "}"
            "QDockWidget::close-button:hover, QDockWidget::float-button:hover {"
            "  background-color: #163D5E;"
            "}"

            // ── Push button (default ghost style) ─────────────────────────
            "QPushButton {"
            "  background-color: #12334F;"
            "  color: #E6F4F9;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "  padding: 8px 16px;"
            "  border-radius: 6px;"
            "  border: 1px solid rgba(4, 138, 191, 0.28);"
            "  outline: none;"
            "}"
            "QPushButton:hover {"
            "  background-color: #163D5E;"
            "  color: #048ABF;"
            "  border-color: rgba(4, 138, 191, 0.55);"
            "}"
            "QPushButton:pressed {"
            "  background-color: #1A4870;"
            "  color: #048ABF;"
            "}"
            "QPushButton:disabled {"
            "  background-color: #0E2940;"
            "  color: rgba(230, 244, 249, 0.30);"
            "  border-color: rgba(4, 138, 191, 0.12);"
            "}"
            "QPushButton:flat {"
            "  background-color: transparent;"
            "  border: none;"
            "  color: #048ABF;"
            "}"
            "QPushButton:flat:hover {"
            "  background-color: rgba(18, 51, 79, 0.8);"
            "}"

            // ── Group box ──────────────────────────────────────────────────
            "QGroupBox {"
            "  background-color: #0E2940;"
            "  border: 1px solid rgba(4, 138, 191, 0.18);"
            "  border-radius: 8px;"
            "  margin-top: 18px;"
            "  padding-top: 6px;"
            "  color: #8AB8CC;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "}"
            "QGroupBox::title {"
            "  subcontrol-origin: margin;"
            "  subcontrol-position: top left;"
            "  padding: 0 8px;"
            "  left: 12px;"
            "  color: #8AB8CC;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.18em;"
            "}"

            // ── Line edit / spin box ───────────────────────────────────────
            "QLineEdit, QDoubleSpinBox, QSpinBox, QTextEdit {"
            "  background-color: #0E2940;"
            "  color: #E6F4F9;"
            "  border: 1px solid rgba(4, 138, 191, 0.32);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 13px;"
            "  selection-background-color: rgba(4, 138, 191, 0.28);"
            "}"
            "QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QTextEdit:focus {"
            "  background-color: #12334F;"
            "  border-color: rgba(4, 138, 191, 0.55);"
            "}"
            "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button,"
            "QSpinBox::up-button, QSpinBox::down-button {"
            "  background-color: #12334F;"
            "  border: none;"
            "  width: 18px;"
            "}"
            "QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover,"
            "QSpinBox::up-button:hover, QSpinBox::down-button:hover {"
            "  background-color: #163D5E;"
            "}"

            // ── Combo box ──────────────────────────────────────────────────
            "QComboBox {"
            "  background-color: #0E2940;"
            "  color: #E6F4F9;"
            "  border: 1px solid rgba(4, 138, 191, 0.32);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 13px;"
            "}"
            "QComboBox:hover { border-color: rgba(4, 138, 191, 0.55); }"
            "QComboBox:focus { border-color: rgba(4, 138, 191, 0.70); }"
            "QComboBox::drop-down {"
            "  border: none;"
            "  width: 24px;"
            "  subcontrol-origin: padding;"
            "  subcontrol-position: center right;"
            "}"
            "QComboBox QAbstractItemView {"
            "  background-color: #12334F;"
            "  color: #E6F4F9;"
            "  border: 1px solid rgba(4, 138, 191, 0.32);"
            "  border-radius: 4px;"
            "  selection-background-color: #163D5E;"
            "  selection-color: #048ABF;"
            "  outline: none;"
            "  padding: 4px;"
            "}"

            // ── List / tree widget ─────────────────────────────────────────
            "QListWidget, QTreeWidget, QTreeView {"
            "  background-color: #0E2940;"
            "  color: #E6F4F9;"
            "  border: none;"
            "  outline: none;"
            "}"
            "QListWidget::item, QTreeWidget::item {"
            "  padding: 8px 12px;"
            "  border-radius: 4px;"
            "}"
            "QListWidget::item:selected, QTreeWidget::item:selected {"
            "  background-color: #12334F;"
            "  color: #048ABF;"
            "}"
            "QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected {"
            "  background-color: rgba(18, 51, 79, 0.65);"
            "}"

            // ── Table view ─────────────────────────────────────────────────
            "QTableView {"
            "  background-color: #0E2940;"
            "  color: #E6F4F9;"
            "  gridline-color: rgba(4, 138, 191, 0.14);"
            "  border: none;"
            "  outline: none;"
            "  alternate-background-color: #091E30;"
            "}"
            "QTableView::item { padding: 6px 8px; }"
            "QTableView::item:selected { background-color: #12334F; color: #048ABF; }"
            "QHeaderView { background-color: #12334F; border: none; }"
            "QHeaderView::section {"
            "  background-color: #12334F;"
            "  color: #8AB8CC;"
            "  padding: 7px 10px;"
            "  font-family: \"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif;"
            "  font-size: 10px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.10em;"
            "  border: none;"
            "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
            "  border-right: 1px solid rgba(4, 138, 191, 0.10);"
            "}"
            "QHeaderView::section:hover { background-color: #163D5E; color: #048ABF; }"

            // ── Scroll bars ────────────────────────────────────────────────
            "QScrollBar:vertical {"
            "  background-color: transparent;"
            "  width: 8px; border: none; margin: 0;"
            "}"
            "QScrollBar::handle:vertical {"
            "  background-color: rgba(4, 138, 191, 0.40);"
            "  border-radius: 4px; min-height: 24px;"
            "}"
            "QScrollBar::handle:vertical:hover { background-color: #048ABF; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "  background: none; border: none; height: 0;"
            "}"
            "QScrollBar:horizontal {"
            "  background-color: transparent;"
            "  height: 8px; border: none; margin: 0;"
            "}"
            "QScrollBar::handle:horizontal {"
            "  background-color: rgba(4, 138, 191, 0.40);"
            "  border-radius: 4px; min-width: 24px;"
            "}"
            "QScrollBar::handle:horizontal:hover { background-color: #048ABF; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
            "  background: none; border: none; width: 0;"
            "}"

            // ── Scroll area ────────────────────────────────────────────────
            "QScrollArea { background-color: #091E30; border: none; }"
            "QScrollArea > QWidget { background-color: transparent; }"

            // ── Slider ─────────────────────────────────────────────────────
            "QSlider::groove:horizontal {"
            "  background-color: #163D5E;"
            "  height: 4px; border-radius: 2px;"
            "}"
            "QSlider::handle:horizontal {"
            "  background-color: #048ABF;"
            "  width: 14px; height: 14px;"
            "  margin: -5px 0; border-radius: 7px;"
            "  border: 2px solid #0E2940;"
            "}"
            "QSlider::handle:horizontal:hover { background-color: #5BB8D8; }"
            "QSlider::sub-page:horizontal {"
            "  background-color: #0367A6;"
            "  height: 4px; border-radius: 2px;"
            "}"

            // ── Splitter ───────────────────────────────────────────────────
            "QSplitter::handle { background-color: rgba(4, 138, 191, 0.20); }"
            "QSplitter::handle:horizontal { width: 1px; }"
            "QSplitter::handle:vertical   { height: 1px; }"

            // ── Radio / check box ──────────────────────────────────────────
            "QRadioButton, QCheckBox {"
            "  color: #E6F4F9; spacing: 8px; background-color: transparent;"
            "}"
            "QRadioButton::indicator {"
            "  width: 16px; height: 16px; border-radius: 8px;"
            "  border: 2px solid #245A80; background-color: #0E2940;"
            "}"
            "QCheckBox::indicator {"
            "  width: 16px; height: 16px; border-radius: 3px;"
            "  border: 2px solid #245A80; background-color: #0E2940;"
            "}"
            "QRadioButton::indicator:checked, QCheckBox::indicator:checked {"
            "  background-color: #048ABF; border-color: #048ABF;"
            "}"
            "QRadioButton::indicator:hover, QCheckBox::indicator:hover {"
            "  border-color: #048ABF;"
            "}"

            // ── Labels ─────────────────────────────────────────────────────
            "QLabel { background-color: transparent; color: #E6F4F9; }"

            // ── Tooltip ────────────────────────────────────────────────────
            "QToolTip {"
            "  background-color: #12334F;"
            "  color: #E6F4F9;"
            "  border: 1px solid rgba(4, 138, 191, 0.35);"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 12px;"
            "}"

            // ── Web engine ─────────────────────────────────────────────────
            "QWebEngineView { background-color: #091E30; }");
}

QLabel *createVisivoLogoLabel(int height, QWidget *parent)
{
    // Try the SVG resource first — crisp at any DPI.
    QSvgRenderer renderer(QStringLiteral(":/icons/visivo_logo_white.svg"));
    if (renderer.isValid()) {
        const QSize svgSize = renderer.defaultSize();
        // Scale width to match requested height while preserving aspect ratio.
        const int w = (svgSize.height() > 0)
                ? qRound(svgSize.width() * static_cast<double>(height) / svgSize.height())
                : height * 2;
        QPixmap pm(w, height);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        renderer.render(&painter);
        painter.end();

        auto *lbl = new QLabel(parent);
        lbl->setPixmap(pm);
        lbl->setFixedSize(w, height);
        lbl->setStyleSheet(u"background: transparent; border: none;"_s);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        return lbl;
    }

    // SVG failed — fall back to the bundled 128-px PNG.
    QPixmap fallback(QStringLiteral(":/icons/VisIVO_128.png"));
    if (!fallback.isNull()) {
        const QPixmap scaled = fallback.scaledToHeight(height, Qt::SmoothTransformation);
        auto *lbl = new QLabel(parent);
        lbl->setPixmap(scaled);
        lbl->setFixedSize(scaled.width(), scaled.height());
        lbl->setStyleSheet(u"background: transparent; border: none;"_s);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        return lbl;
    }

    qWarning("[VisivoTheme] createVisivoLogoLabel: both SVG and PNG resources failed to load");
    return nullptr;
}

void applyTo(QApplication *app)
{
    // ── 1. Register bundled fonts first ───────────────────────────────────────
    // Must happen before any QFont or QSS that references "Lato".
    loadBundledFonts();

    // ── 2. Set Lato as the application default font ───────────────────────────
    // This covers widgets that bypass QSS font-family (native delegates, VTK
    // overlays, platform renderers). Lato Regular at 13px matches the brand body
    // size and is a safe, readable default.
    QFont appFont(QStringLiteral("Lato"));
    appFont.setPointSize(13);
    appFont.setWeight(QFont::Normal);
    app->setFont(appFont);

    // ── 3. QPalette ───────────────────────────────────────────────────────────
    // Ensures widgets that skip QSS get coherent VisIVO-navy colours.
    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(kBackground));
    pal.setColor(QPalette::WindowText,      QColor(kOnSurface));
    pal.setColor(QPalette::Base,            QColor(kSurfaceContainerLow));
    pal.setColor(QPalette::AlternateBase,   QColor(kSurface));
    pal.setColor(QPalette::Text,            QColor(kOnSurface));
    pal.setColor(QPalette::BrightText,      QColor(kPrimary));
    pal.setColor(QPalette::Button,          QColor(kSurfaceContainerHigh));
    pal.setColor(QPalette::ButtonText,      QColor(kOnSurface));
    pal.setColor(QPalette::Highlight,       QColor(kSurfaceBright));
    pal.setColor(QPalette::HighlightedText, QColor(kPrimary));
    pal.setColor(QPalette::Link,            QColor(kPrimary));
    pal.setColor(QPalette::LinkVisited,     QColor(kPrimaryDim));
    pal.setColor(QPalette::ToolTipBase,     QColor(kSurfaceContainerHigh));
    pal.setColor(QPalette::ToolTipText,     QColor(kOnSurface));
    pal.setColor(QPalette::PlaceholderText, QColor(kOnSurfaceVariant));
    pal.setColor(QPalette::Mid,             QColor(kOutlineVariant));
    pal.setColor(QPalette::Midlight,        QColor(kSurfaceContainer));
    pal.setColor(QPalette::Dark,            QColor(kSurfaceContainerLow));
    pal.setColor(QPalette::Shadow,          QColor(kBackground));
    app->setPalette(pal);

    // ── 4. QSS ────────────────────────────────────────────────────────────────
    app->setStyleSheet(globalStyleSheet());
}

} // namespace VisivoTheme
