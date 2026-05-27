#ifndef ToolDialogStyle_h
#define ToolDialogStyle_h

// ---------------------------------------------------------------------------
// ToolDialogStyle — shared visual chrome for parameter-input dialogs
// (Moment Map Settings, Baseline Subtraction, Line-width Map, Stack
// Spectral Cubes, Noise Region picker, …).
//
// Each tool dialog used to roll its own QFormLayout with default Qt
// styling, which on macOS dark mode produced flat dark inputs with no
// visible dropdown arrow and no visual grouping. The MomentMap redesign
// established the new look (section headers + themed inputs + chevron on
// QComboBox + muted help text + footer divider) — this header extracts
// that look so the other dialogs share exactly the same chrome with no
// CSS copy/paste.
//
// Usage:
//     using namespace ToolDialogStyle;
//     dialog.setStyleSheet(inputCss());
//     auto *hdr = new QLabel(u"COMPUTE"_s); hdr->setStyleSheet(sectionCss());
//     auto *hint = new QLabel(...);         hint->setStyleSheet(helperCss());
//     // and inside a section, addSection(dialog->layout(), "Channel range")
//     //  returns a QFormLayout you populate with form rows.
// ---------------------------------------------------------------------------

#include "VisivoTheme.h"

#include <QFormLayout>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace ToolDialogStyle {

/// CSS applied at dialog level — themes QComboBox / QSpinBox / QDoubleSpinBox
/// so they share a single border, hover, focus, and (critically) the
/// chevron asset for the combobox drop-down arrow.
inline QString inputCss()
{
    const QString border    = QString::fromLatin1(VisivoTheme::kOutline());
    const QString surface   = QString::fromLatin1(VisivoTheme::kSurfaceContainerLow());
    const QString onSurface = QString::fromLatin1(VisivoTheme::kOnSurface());
    const QString primary   = QString::fromLatin1(VisivoTheme::kPrimary());
    return QString::fromLatin1(
            "QComboBox, QSpinBox, QDoubleSpinBox {"
            "  background: %2; color: %3;"
            "  border: 1px solid %1; border-radius: 4px;"
            "  padding: 5px 10px; min-height: 18px;"
            "}"
            "QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: %4; }"
            "QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: %4; }"
            "QSpinBox::up-button, QDoubleSpinBox::up-button {"
            "  subcontrol-origin: border; subcontrol-position: top right;"
            "  width: 20px; border-left: 1px solid %1;"
            "  border-top-right-radius: 4px;"
            "  background: %2;"
            "}"
            "QSpinBox::down-button, QDoubleSpinBox::down-button {"
            "  subcontrol-origin: border; subcontrol-position: bottom right;"
            "  width: 20px; border-left: 1px solid %1;"
            "  border-bottom-right-radius: 4px;"
            "  background: %2;"
            "}"
            "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {"
            "  image: url(:/icons/chevron_up.svg);"
            "  width: 8px; height: 6px;"
            "}"
            "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
            "  image: url(:/icons/chevron_down.svg);"
            "  width: 8px; height: 6px;"
            "}"
            "QComboBox::drop-down {"
            "  subcontrol-origin: padding; subcontrol-position: top right;"
            "  width: 24px; border-left: 1px solid %1;"
            "  border-top-right-radius: 4px; border-bottom-right-radius: 4px;"
            "}"
            "QComboBox::down-arrow {"
            "  image: url(:/icons/chevron_down.svg);"
            "  width: 10px; height: 7px;"
            "}"
            "QComboBox QAbstractItemView {"
            "  background: %2; color: %3; border: 1px solid %1;"
            "  selection-background-color: %4; selection-color: white;"
            "  padding: 2px;"
            "}")
            .arg(border, surface, onSurface, primary);
}

/// Section-header label: small-caps brand-blue, used between QFormLayouts
/// to group related fields visually (COMPUTE / CHANNEL RANGE / MASKING).
inline QString sectionCss()
{
    return QString::fromLatin1(
                   "color: %1; font-size: 10px; font-weight: 700;"
                   "letter-spacing: 0.10em; text-transform: uppercase;"
                   "margin-top: 4px; margin-bottom: 2px;")
            .arg(QString::fromLatin1(VisivoTheme::kPrimary()));
}

/// Muted helper-text label, sits under inputs to explain the option in
/// scientific terms (e.g. "Sum I·Δv over the channel range — total line
/// flux per pixel.").
inline QString helperCss()
{
    return QString::fromLatin1("color: %1; font-size: 11px; padding-left: 2px;")
            .arg(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()));
}

/// Tool-menu breadcrumb shown at the top of every parameter dialog so the
/// user always knows which Tools-menu entry spawned the window they're
/// looking at. Rendered small / muted / uppercase so it doesn't compete
/// visually with the window's actual title bar.
///
/// Usage from a dialog ctor:
///     auto *root = new QVBoxLayout(this);
///     root->addWidget(ToolDialogStyle::makeBreadcrumb(this, u"Estimate Noise"_s));
///     // ... rest of the dialog ...
inline QLabel *makeBreadcrumb(QWidget *parent, const QString &toolName)
{
    auto *lbl = new QLabel(QStringLiteral("Tools  ›  %1").arg(toolName), parent);
    lbl->setStyleSheet(
            QStringLiteral(
                    "color: %1; font-size: 10px; font-weight: 700;"
                    "letter-spacing: 0.10em; text-transform: uppercase;"
                    "padding-bottom: 6px;")
                    .arg(QString::fromLatin1(VisivoTheme::kOnSurfaceVariant())));
    return lbl;
}

/// Add a section header + a fresh QFormLayout to @p outerLayout. Returns
/// the form, ready for `form->addRow(label, widget)`. Removes the
/// boilerplate from each dialog's constructor.
inline QFormLayout *addSection(QVBoxLayout *outerLayout, const QString &title,
                                QWidget *parent = nullptr)
{
    auto *hdr = new QLabel(title, parent);
    hdr->setStyleSheet(sectionCss());
    outerLayout->addWidget(hdr);
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);
    outerLayout->addLayout(form);
    return form;
}

}  // namespace ToolDialogStyle

#endif  // ToolDialogStyle_h
