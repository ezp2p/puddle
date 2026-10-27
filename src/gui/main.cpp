// Puddle: desktop app. Qt Quick UI over the same core server that puddled runs.
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include "app/backend.h"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("Puddle");
    app.setOrganizationName("sloporation");
    app.setWindowIcon(QIcon(":/qt/qml/Puddle/src/gui/assets/puddle.png"));
    // Closing the window hides to the tray; the app keeps serving until Quit.
    app.setQuitOnLastWindowClosed(false);

    // "Basic" is fully restylable; the native macOS/Windows styles restrict customization.
    QQuickStyle::setStyle("Basic");

    Backend backend; // declared before the engine so it outlives the QML objects using it
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.setInitialProperties({ { "backend", QVariant::fromValue(&backend) } });
    engine.loadFromModule("Puddle", "Main");

    return app.exec();
}
