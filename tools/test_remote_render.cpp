/**
 * @file
 * @brief Standalone test harness for ``RemoteRenderWidget``.
 *
 * Skips the entire VisIVO desktop GUI so you can iterate on the
 * server-side rendering pipeline in isolation. Talks to a backend
 * over WebSocket, displays the JPEG frames, sends camera commands
 * on mouse drag / wheel / resize.
 *
 * Usage (assumes an active SSR session already created via curl):
 *
 *     # 1. Start the backend on the cluster (single-GPU enough)
 *     bash scripts/visivo_slurm_tunnel.sh ... --port 8010 ...
 *
 *     # 2. Open a dataset + create a render session via REST
 *     DSID=$(curl -sf -X POST -H "X-Visivo-Token: $TOKEN" \
 *         -H 'Content-Type: application/json' \
 *         -d '{"path":"/mnt/beegfs/fvitello/data/WALLABY.fits"}' \
 *         http://localhost:8010/v1/datasets/open | jq -r .dataset_id)
 *     SID=$(curl -sf -X POST -H "X-Visivo-Token: $TOKEN" \
 *         -H 'Content-Type: application/json' \
 *         -d "{\"dataset_id\":\"$DSID\"}" \
 *         http://localhost:8010/v1/render/create_session | jq -r .session_id)
 *
 *     # 3. Run this test app
 *     ./test_remote_render http://localhost:8010 "$TOKEN" "$SID"
 *
 * Drag the volume with the mouse, scroll to zoom, resize the window.
 * The status bar at the bottom shows live FPS + connection state.
 */
#include "../src/gui/RemoteRenderWidget.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>


int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("test_remote_render"));
    QApplication::setApplicationVersion(QStringLiteral("0.1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
            QStringLiteral("Standalone test harness for VisIVO's "
                           "server-side render WebSocket channel."));
    parser.addPositionalArgument(
            QStringLiteral("baseUrl"),
            QStringLiteral("Backend URL, e.g. http://localhost:8010"));
    parser.addPositionalArgument(
            QStringLiteral("token"),
            QStringLiteral("Backend bearer token (X-Visivo-Token)"));
    parser.addPositionalArgument(
            QStringLiteral("sessionId"),
            QStringLiteral("Active render session_id "
                           "(create via POST /v1/render/create_session)"));
    parser.addHelpOption();
    parser.process(app);

    const auto args = parser.positionalArguments();
    if (args.size() != 3) {
        parser.showHelp(2);
        return 2;
    }
    const QString baseUrl   = args.at(0);
    const QString token     = args.at(1);
    const QString sessionId = args.at(2);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("VisIVO — Remote Render Test"));
    window.resize(1024, 768);

    auto *widget = new RemoteRenderWidget(&window);
    window.setCentralWidget(widget);

    // Live status bar: connection state on the left, FPS on the right.
    auto *connLabel = new QLabel(QStringLiteral("Idle"), &window);
    auto *fpsLabel  = new QLabel(QStringLiteral("FPS: —"), &window);
    fpsLabel->setMinimumWidth(120);
    window.statusBar()->addWidget(connLabel, /*stretch=*/1);
    window.statusBar()->addPermanentWidget(fpsLabel);

    QObject::connect(widget, &RemoteRenderWidget::connectionStateChanged,
                     connLabel, &QLabel::setText);
    QObject::connect(widget, &RemoteRenderWidget::framesPerSecondChanged,
                     fpsLabel, [fpsLabel](double fps) {
                         fpsLabel->setText(
                                 QStringLiteral("FPS: %1").arg(fps, 0, 'f', 1));
                     });

    // Kick off the connection. The widget hides everything behind a
    // "Connecting…" overlay until the first JPEG frame lands.
    widget->connectToSession(baseUrl, sessionId, token);
    window.show();

    return app.exec();
}
