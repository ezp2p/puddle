// Puddle: desktop app. Qt Quick UI over the same core server that puddled runs.
#ifdef PUDDLE_HAVE_WIDGETS
#include <QApplication> // the tray icon is a widget on Linux and Windows
using App = QApplication;
#else
#include <QGuiApplication>
using App = QGuiApplication;
#endif
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include "app/backend.h"
#include "app/composer.h"
#include "app/qrimage.h"

int main(int argc, char **argv)
{
    App app(argc, argv);
    app.setApplicationName("Puddle");
    app.setOrganizationName("ezp2p");
    app.setWindowIcon(QIcon(":/qt/qml/Puddle/src/gui/assets/puddle.png"));
    // Closing the window hides to the tray; the app keeps serving until Quit.
    app.setQuitOnLastWindowClosed(false);

    // "Basic" is fully restylable; the native macOS/Windows styles restrict customization.
    QQuickStyle::setStyle("Basic");

    qmlRegisterType<Composer>("Puddle.Files", 1, 0, "Composer"); // the files waiting to go with a message

    Backend backend; // declared before the engine so it outlives the QML objects using it
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("qr"), new QrImageProvider); // the engine owns it
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.setInitialProperties({ { "backend", QVariant::fromValue(&backend) } });
    engine.loadFromModule("Puddle", "Main");

    return app.exec();
}
