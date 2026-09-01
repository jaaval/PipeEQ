#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>

#include "dbus_client.h"
#include "fake_backend.h"
#include "main_window.h"
#include "theme/theme.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("PipeEQ");
    app.setApplicationDisplayName("PipeEQ");

    QCommandLineParser parser;
    parser.setApplicationDescription("Per-channel audio routing, volume and EQ for PipeWire.");
    parser.addHelpOption();

    QCommandLineOption demoOption(
        "demo", "Run against a fixed fake topology with synthetic levels, with no daemon, no "
                "session bus and no PipeWire. Intended for UI work and screenshots.");
    parser.addOption(demoOption);
    QCommandLineOption geometryOption("geometry", "Window size as WxH, e.g. 1280x800.", "WxH");
    parser.addOption(geometryOption);
    parser.process(app);

    // Before any widget exists: the palette has to be in place when widgets
    // read it at construction.
    pipeeq::theme::install(app);

    pipeeq::Backend* backend = nullptr;
    if (parser.isSet(demoOption)) {
        backend = new pipeeq::FakeBackend(&app);
    } else {
        backend = new pipeeq::DbusClient(&app);
    }

    pipeeq::MainWindow window(backend);

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
    return app.exec();
}
