#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QTimer>

#include "main_window.h"
#include "model/app_state.h"
#include "theme/theme.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("PipeEQ");
    app.setApplicationDisplayName("PipeEQ");
    // QSettings derives its file path from these; without them it writes to an
    // "Unknown Organization" directory.
    app.setOrganizationName("PipeEQ");
    app.setOrganizationDomain("pipeeq.local");

    QCommandLineParser parser;
    parser.setApplicationDescription("Per-channel audio routing, volume and EQ for PipeWire.");
    parser.addHelpOption();

    QCommandLineOption demoOption(
        "demo", "Run against a fixed fake topology with synthetic levels, with no daemon, no "
                "session bus and no PipeWire. Intended for UI work and screenshots.");
    parser.addOption(demoOption);
    QCommandLineOption geometryOption("geometry", "Window size as WxH, e.g. 1280x800.", "WxH");
    parser.addOption(geometryOption);
    QCommandLineOption openEqOption(
        "open-eq", "Start on the EQ editor page. For screenshots and scripted checks, which "
                   "cannot otherwise reach a page that needs a click.");
    parser.addOption(openEqOption);
    parser.process(app);

    // Before any widget exists: the palette has to be in place when widgets
    // read it at construction.
    pipeeq::theme::install(app);

    // The store owns the backend and the thread it runs on; which backend it
    // creates is the only thing --demo decides.
    pipeeq::AppState state(parser.isSet(demoOption));
    pipeeq::MainWindow window(&state);

    if (parser.isSet(geometryOption)) {
        // A fixed size makes screenshots comparable between runs, which is the
        // only reason this option exists.
        const QStringList parts = parser.value(geometryOption).split('x');
        bool widthOk = false;
        bool heightOk = false;
        const int width = parts.value(0).toInt(&widthOk);
        const int height = parts.value(1).toInt(&heightOk);
        if (widthOk && heightOk && width > 0 && height > 0) {
            window.resize(width, height);
        }
    }

    window.show();
    // Metering is armed once there is something on screen to show it on. The
    // daemon's lease expires by itself, so the store re-arms while this is set.
    state.setMeteringEnabled(true);

    if (parser.isSet(openEqOption)) {
        // Deferred: the first snapshot has to arrive before there is a selection
        // for the editor to show.
        QTimer::singleShot(1200, &window, &pipeeq::MainWindow::showEqEditor);
    }
    return app.exec();
}
