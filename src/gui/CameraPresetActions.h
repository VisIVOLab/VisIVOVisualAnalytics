#pragma once

// Shared Camera menu for every 3-D viewer. The FITS cube viewer historically
// provided these six viewpoints; this helper gives programmatic viewers the
// same labels, icons and shortcuts without duplicating their camera maths.

#include <QAction>
#include <QIcon>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>

#include <vtkCamera.h>
#include <vtkRenderer.h>

#include <cmath>
#include <functional>

namespace visivo {

inline void applyCameraPreset(vtkRenderer *renderer, double azimuth, double elevation)
{
    if (!renderer)
        return;
    const double a = azimuth * 3.14159265358979323846 / 180.0;
    const double e = elevation * 3.14159265358979323846 / 180.0;
    const double back[3]{ std::sin(a), 0.0, std::cos(a) };
    const double direction[3]{ std::cos(e) * back[0], std::sin(e),
                               std::cos(e) * back[2] };
    const double up[3]{ -std::sin(e) * back[0], std::cos(e),
                        -std::sin(e) * back[2] };
    vtkCamera *camera = renderer->GetActiveCamera();
    const double *focal = camera->GetFocalPoint();
    camera->SetPosition(focal[0] + direction[0], focal[1] + direction[1],
                        focal[2] + direction[2]);
    camera->SetViewUp(up);
    renderer->ResetCamera();
    renderer->ResetCameraClippingRange();
}

inline QMenu *installCameraPresetMenu(
        QMainWindow *window, const std::function<void(double, double)> &apply)
{
    auto *menu = new QMenu(QStringLiteral("Camera"), window);
    struct Preset {
        const char *label;
        const char *icon;
        const char *shortcut;
        double azimuth;
        double elevation;
    };
    static constexpr Preset presets[]{
        { "Front", ":/icons/PIC_FRONT.png", "Ctrl+1", 0.0, 0.0 },
        { "Back", ":/icons/PIC_BACK.png", "Ctrl+2", -180.0, 0.0 },
        { "Left", ":/icons/PIC_LEFT.png", "Ctrl+3", -90.0, 0.0 },
        { "Right", ":/icons/PIC_RIGHT.png", "Ctrl+4", 90.0, 0.0 },
        { "Top", ":/icons/PIC_TOP.png", "Ctrl+5", 0.0, 90.0 },
        { "Bottom", ":/icons/PIC_BOTTOM.png", "Ctrl+6", 0.0, -90.0 },
    };
    for (const Preset &preset : presets) {
        auto *action = menu->addAction(QIcon(QString::fromLatin1(preset.icon)),
                                       QString::fromLatin1(preset.label));
        action->setShortcut(QKeySequence(QString::fromLatin1(preset.shortcut)));
        action->setShortcutContext(Qt::WindowShortcut);
        action->setToolTip(QStringLiteral("Look at the data from %1 (%2).")
                                   .arg(action->text().toLower(),
                                        action->shortcut().toString(QKeySequence::NativeText)));
        QObject::connect(action, &QAction::triggered, window,
                         [apply, preset]() { apply(preset.azimuth, preset.elevation); });
    }
    // Right after View (the camera is a way of viewing), else before Window or
    // Help, else last: never ahead of File, where the platform expects it.
    const QList<QAction *> bar = window->menuBar()->actions();
    const auto title = [](const QAction *action) {
        return action->text().remove(QLatin1Char('&')).trimmed();
    };
    QAction *before = nullptr;
    for (qsizetype i = 0; i < bar.size(); ++i) {
        if (title(bar[i]) == QLatin1String("View")) {
            before = i + 1 < bar.size() ? bar[i + 1] : nullptr;
            break;
        }
        if (!before && (title(bar[i]) == QLatin1String("Window")
                        || title(bar[i]) == QLatin1String("Help")))
            before = bar[i];
    }
    window->menuBar()->insertMenu(before, menu);
    return menu;
}

} // namespace visivo
