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

// ── Brand palette — VisIVO Brand Guidelines §Primary Colors ──────────────────
inline constexpr auto kNavy        = "#0E2940";   // primary dark
inline constexpr auto kBlue        = "#0367A6";   // corporate blue accent
inline constexpr auto kBlueBright  = "#048ABF";   // bright blue / hover

// ── Surface palette — navy-based dark shell ───────────────────────────────────
inline constexpr auto kBackground              = "#091E30";
inline constexpr auto kSurface                 = "#091E30";
inline constexpr auto kSurfaceContainerLow     = "#0E2940";
inline constexpr auto kSurfaceContainer        = "#12334F";
inline constexpr auto kSurfaceContainerHigh    = "#163D5E";
inline constexpr auto kSurfaceContainerHighest = "#1A4870";
inline constexpr auto kSurfaceBright           = "#1F5280";
inline constexpr auto kSurfaceVariant          = "#1A4870";

// ── Primary accent (interactive elements) ─────────────────────────────────────
inline constexpr auto kPrimary         = "#048ABF";   // bright blue — buttons, icons, links
inline constexpr auto kPrimaryDim      = "#0367A6";   // corporate blue — hover dim
inline constexpr auto kOnPrimary       = "#FFFFFF";   // white text on filled buttons
inline constexpr auto kPrimaryFixed    = "#5BB8D8";   // lighter blue for pressed state
inline constexpr auto kPrimaryFixedDim = "#3AAECF";

// ── Text ─────────────────────────────────────────────────────────────────────
inline constexpr auto kOnSurface        = "#E6F4F9";   // primary text (VisIVO blue tint)
inline constexpr auto kOnSurfaceVariant = "#8AB8CC";   // secondary / muted text
inline constexpr auto kSecondary        = "#6A9EB8";   // tertiary muted

// ── Borders ───────────────────────────────────────────────────────────────────
inline constexpr auto kOutline        = "#245A80";
inline constexpr auto kOutlineVariant = "#163D5E";

// ── Semantic states ───────────────────────────────────────────────────────────
inline constexpr auto kTertiaryContainer  = "#1A6B45";   // success badge bg
inline constexpr auto kOnTertiaryFixed    = "#C8F0DB";   // success badge text
inline constexpr auto kErrorContainer     = "#7A2020";   // error badge bg
inline constexpr auto kOnErrorContainer   = "#FFB3B3";   // error badge text
inline constexpr auto kWarningContainer   = "#5C3D10";   // warning badge bg
inline constexpr auto kOnWarningContainer = "#FFD08A";   // warning badge text

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

// ── Global theme application ──────────────────────────────────────────────────
QString globalStyleSheet();
void applyTo(QApplication *app);

} // namespace VisivoTheme

#endif // VisivoTheme_h
