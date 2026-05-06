#ifndef VisivoTheme_h
#define VisivoTheme_h

#include <QString>

class QApplication;
class QFrame;
class QLabel;
class QPushButton;
class QWidget;

// ---------------------------------------------------------------------------
// VisivoTheme — VisIVO Brand Design System
//
// Color palette sourced from the official VisIVO Brand Guidelines PDF.
//
// Primary palette (blues):
//   kNavy      #0E2940   deep navy — base background / darkest surface
//   kBlue      #0367A6   corporate blue — primary interactive accent
//   kBlueBright#048ABF   bright blue — hover / active / secondary accent
//   kBlueTint  #E6F4F9   very light blue tint (reference only, light mode)
//
// Surface hierarchy (dark application shell, navy-based):
//   kSurface                  #091E30   deepest background
//   kSurfaceContainerLow      #0E2940   base navy (brand primary dark)
//   kSurfaceContainer         #12334F   standard panels
//   kSurfaceContainerHigh     #163D5E   elevated / active widgets
//   kSurfaceContainerHighest  #1A4870   floating / selected elements
//   kSurfaceBright            #1F5280   hover / focus surfaces
//
// Typography:
//   kBrandFamily   Lato (bundled) — primary brand typeface for all UI
//   kDataFamily    Lato (same) — Inter only as optional system fallback
//
// Usage:
//   VisivoTheme::applyTo(qApp);   ← call in main() after QApplication created
// ---------------------------------------------------------------------------
namespace VisivoTheme {

// ── Theme mode tracking ───────────────────────────────────────────────────────
// Set by applyDarkTo() / applyLightTo(); read by the color functions below.
// All colors that vary between dark and light are returned by inline accessors
// so widgets built via the factory functions or composing inline stylesheets
// automatically pick up the right palette at construction time.
//
// NOTE: changing the mode after widgets are constructed only updates the
//       global QSS — already-constructed widgets keep their inline styles.
//       Recreate them (or restart) to fully refresh.
enum class Mode { Dark, Light };
void setMode(Mode m);
Mode currentMode();
inline bool isDark() { return currentMode() == Mode::Dark; }

// ── Brand palette — VisIVO Brand Guidelines §Primary Colors ──────────────────
// Brand colors are theme-invariant: same blue on dark and light shells.
inline constexpr auto kNavy       = "#0E2940";
inline constexpr auto kBlue       = "#0367A6";
inline constexpr auto kBlueBright = "#048ABF";

// ── Surface palette ───────────────────────────────────────────────────────────
// Returns a hex color (as a static C-string) appropriate for the active theme.
inline const char *kBackground() {
    return isDark() ? "#091E30" : "#F2F6FA";
}
inline const char *kSurface() {
    return isDark() ? "#091E30" : "#FFFFFF";
}
inline const char *kSurfaceContainerLow() {
    return isDark() ? "#0E2940" : "#EBF2F8";
}
inline const char *kSurfaceContainer() {
    return isDark() ? "#12334F" : "#FFFFFF";
}
inline const char *kSurfaceContainerHigh() {
    return isDark() ? "#163D5E" : "#E0ECF5";
}
inline const char *kSurfaceContainerHighest() {
    return isDark() ? "#1A4870" : "#D5E6F2";
}
inline const char *kSurfaceBright() {
    return isDark() ? "#1F5280" : "#FFFFFF";
}
inline const char *kSurfaceVariant() {
    return isDark() ? "#1A4870" : "#D5E6F2";
}

// ── Primary accent (theme-invariant brand blues) ──────────────────────────────
inline const char *kPrimary()         { return "#048ABF"; }
inline const char *kPrimaryDim()      { return "#0367A6"; }
inline const char *kOnPrimary()       { return "#FFFFFF"; }
inline const char *kPrimaryFixed()    { return "#5BB8D8"; }
inline const char *kPrimaryFixedDim() { return "#3AAECF"; }

// ── Text ──────────────────────────────────────────────────────────────────────
inline const char *kOnSurface() {
    return isDark() ? "#E6F4F9" : "#1A2B3C";
}
inline const char *kOnSurfaceVariant() {
    return isDark() ? "#8AB8CC" : "#5A7A8F";
}
inline const char *kSecondary() {
    return isDark() ? "#6A9EB8" : "#5A7A8F";
}

// ── Borders ───────────────────────────────────────────────────────────────────
inline const char *kOutline() {
    return isDark() ? "#245A80" : "#AABFCC";
}
inline const char *kOutlineVariant() {
    return isDark() ? "#163D5E" : "#C5D5E0";
}

// ── Semantic states ───────────────────────────────────────────────────────────
inline const char *kTertiaryContainer()  {
    return isDark() ? "#1A6B45" : "#D7F2E2";
}
inline const char *kOnTertiaryFixed()    {
    return isDark() ? "#C8F0DB" : "#1A6B45";
}
inline const char *kErrorContainer()     {
    return isDark() ? "#7A2020" : "#FBE2E2";
}
inline const char *kOnErrorContainer()   {
    return isDark() ? "#FFB3B3" : "#7A2020";
}
inline const char *kWarningContainer()   {
    return isDark() ? "#5C3D10" : "#FCEAC7";
}
inline const char *kOnWarningContainer() {
    return isDark() ? "#FFD08A" : "#5C3D10";
}

// ── Typography — Lato as the primary brand typeface ───────────────────────────
// Lato is bundled via Qt resources. The fallback chain ensures graceful
// degradation on platforms where resource loading fails.
inline constexpr auto kBrandFamily =
    "\"Lato\", \"Helvetica Neue\", \"Arial\", sans-serif";

// Data/body alias — still Lato; Inter listed as system fallback only.
inline constexpr auto kBodyFamily =
    "\"Lato\", \"Inter\", \"Helvetica Neue\", \"Segoe UI\", sans-serif";

// ── Font loading ──────────────────────────────────────────────────────────────
// Registers the bundled Lato TTF files with QFontDatabase.
// Returns true if at least Lato-Regular loaded successfully.
bool loadBundledFonts();

// ── Logo helper ───────────────────────────────────────────────────────────────
// Returns a QLabel containing the VisIVO SVG logo rendered as a QPixmap.
// The logo is scaled to 'height' pixels (preserving aspect ratio).
// Uses the bundled :/branding/visivo_logo.svg resource.
// Falls back to :/icons/VisIVO_128.png if SVG is unavailable.
// Returns nullptr on failure (caller must guard).
QLabel *createVisivoLogoLabel(int height, QWidget *parent = nullptr);

// ── Component factories ───────────────────────────────────────────────────────
QLabel *makeSectionLabel(const QString &text, QWidget *parent = nullptr);

QFrame *makeCard(QWidget *parent = nullptr, bool elevated = false);

enum class BadgeKind { Success, Error, Warning, Neutral };
QLabel *makeBadge(const QString &text, BadgeKind kind = BadgeKind::Neutral,
                  QWidget *parent = nullptr);

QPushButton *makePrimaryButton(const QString &text, QWidget *parent = nullptr);
QPushButton *makeSecondaryButton(const QString &text, QWidget *parent = nullptr);

// ── Stylesheet helpers shared across visualization windows ───────────────────
// These return fresh QStrings keyed off the current theme so call sites get
// the right palette regardless of when they're invoked.

// QLabel — small uppercase section header inside a sidebar card.
QString sidebarHeaderStyleSheet();

// QLabel — secondary smaller header (no bottom border).
QString sidebarSubHeaderStyleSheet();

// QFrame / QGroupBox — outlined card frame around grouped sidebar widgets.
QString sidebarCardStyleSheet();

// QLabel — muted key text in info grids ("RA", "Dec", …).
QString infoKeyStyleSheet();

// QLabel — primary monospace value text in info grids.
QString infoValueStyleSheet();

// QToolButton — sidebar action button with brand-blue hover / checked tint.
QString sidebarToolButtonStyleSheet();

// ── Global theme application ──────────────────────────────────────────────────

// Dark theme (VisIVO navy — canonical baseline).
QString darkStyleSheet();
void applyDarkTo(QApplication *app);

// Light theme (VisIVO brand blues on light backgrounds).
QString lightStyleSheet();
void applyLightTo(QApplication *app);

// Legacy alias — maps to applyDarkTo().
inline void applyTo(QApplication *app) { applyDarkTo(app); }
// Legacy alias — maps to darkStyleSheet().
inline QString globalStyleSheet() { return darkStyleSheet(); }

} // namespace VisivoTheme

#endif // VisivoTheme_h
