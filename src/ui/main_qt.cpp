#include "rag/rag_controller.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QWindow>
#include <QSize>
#include <QRect>
#include <QDebug>
#include <QTimer>
#include <QMetaObject>
#include <QVariant>
#include <QVariantMap>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <csignal>
#include <unistd.h>

#include "../core/chat_engine.h"
#include "../input/global_input.h"
#include "../model/flm_gateway.h"

static QVariantMap loadKindConfig()
{
    QFile file(QStringLiteral(":/config/kinds.json"));

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[Watcher] Cannot open kind config:" << file.fileName();
        return {};
    }

    const QByteArray data = file.readAll();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "[Watcher] JSON parse error in kind config:"
                   << parseError.errorString()
                   << "at offset" << parseError.offset;
        return {};
    }

    if (!doc.isObject()) {
        qWarning() << "[Watcher] Kind config root is not a JSON object";
        return {};
    }

    return doc.object().toVariantMap();
}

int main(int argc, char *argv[])
{
    qputenv("QT_NO_SESSION_MANAGER", "1");

    // Layer-shell нужен, чтобы Polonium/KWin не трогал полноэкранный оверлей.
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");

    QGuiApplication app(argc, argv);
    app.setApplicationName("Watcher");
    app.setOrganizationName("Watcher");
    app.setDesktopFileName("watcher");

    // Watcher живёт как фоновый оверлей.
    app.setQuitOnLastWindowClosed(false);

    // Ctrl+C теперь используется для копирования,
    // поэтому SIGINT игнорируем.
    std::signal(SIGINT, SIG_IGN);

    qDebug() << "[Watcher] Starting application...";

    StreamQueue queue;
    FlmGateway gateway(queue, "127.0.0.1", 52625);

    ChatEngine engine(queue, gateway);

    GlobalInput globalInput;
    if (!globalInput.init()) {
        qWarning() << "[Watcher] Global keyboard input failed";
    }

    QVariantMap kindConfig = loadKindConfig();

    QQmlApplicationEngine overlayEngine;

    overlayEngine.rootContext()->setContextProperty("chatEngine", &engine);
    overlayEngine.rootContext()->setContextProperty("kindConfig", kindConfig);

    rag::RagController *ragCtl = new rag::RagController(qApp);
    overlayEngine.rootContext()->setContextProperty(QStringLiteral("rag"), ragCtl);

    overlayEngine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    if (overlayEngine.rootObjects().isEmpty()) {
        qCritical() << "[Watcher] ERROR: No overlay root objects loaded!";
        return -1;
    }

    QObject *overlayRoot = overlayEngine.rootObjects().constFirst();
    QQuickWindow *overlayWindow = qobject_cast<QQuickWindow *>(overlayRoot);

    if (overlayWindow) {
        overlayWindow->setFlags(
              Qt::Tool
            | Qt::FramelessWindowHint
            | Qt::WindowStaysOnTopHint
            | Qt::BypassWindowManagerHint
        );

        overlayWindow->setColor(Qt::transparent);

        if (auto *screen = QGuiApplication::primaryScreen()) {
            overlayWindow->setGeometry(screen->geometry());
        }

        overlayWindow->setProperty("_q_waylandLayer", "overlay");
        overlayWindow->setProperty("_q_waylandNamespace", "watcher");
        overlayWindow->setProperty("_q_waylandExclusiveZone", 0);
        overlayWindow->setProperty("_q_waylandKeyboardInteractivity", "none");
        overlayWindow->setProperty(
            "_q_waylandAnchor",
            int(Qt::TopEdge | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge)
        );

        overlayWindow->setVisibility(QWindow::Hidden);
    }

    auto setKeyboardInteractivity = [overlayWindow](bool enabled) {
        if (!overlayWindow) {
            return;
        }

        overlayWindow->setProperty(
            "_q_waylandKeyboardInteractivity",
            enabled ? "exclusive" : "none"
        );
    };

    auto toggleChatUi = [overlayRoot, &setKeyboardInteractivity]() {
        if (!overlayRoot) {
            return;
        }

        const bool active = overlayRoot->property("active").toBool();
        const bool fading = overlayRoot->property("fading").toBool();

        if (fading) {
            return;
        }

        if (!active) {
            setKeyboardInteractivity(true);
            QMetaObject::invokeMethod(overlayRoot, "openChatUi");
        } else {
            QMetaObject::invokeMethod(overlayRoot, "closeChatUi");
            setKeyboardInteractivity(false);
        }
    };

    QObject::connect(
        &globalInput,
        &GlobalInput::hotkeyToggle,
        &app,
        toggleChatUi
    );

    QObject::connect(
        &globalInput,
        &GlobalInput::hotkeyInput,
        &app,
        toggleChatUi
    );

    QObject::connect(
        &globalInput,
        &GlobalInput::copyRequested,
        &app,
        [overlayRoot]() {
            if (overlayRoot) {
                QMetaObject::invokeMethod(overlayRoot, "copyCurrentSelection");
            }
        }
    );

    QObject::connect(
        &globalInput,
        &GlobalInput::keyPressed,
        &app,
        [overlayRoot](int scanCode) {
            if (overlayRoot) {
                QMetaObject::invokeMethod(
                    overlayRoot,
                    "createKeyBurstByScanCode",
                    Q_ARG(QVariant, scanCode)
                );
            }
        }
    );

    QObject::connect(
        &globalInput,
        &GlobalInput::quitRequested,
        &app,
        [overlayRoot, &app]() {
            qDebug() << "[Watcher] Quit requested by Ctrl+Alt+Esc";

            if (overlayRoot) {
                QMetaObject::invokeMethod(overlayRoot, "forceCloseChatUi");
            }

            QCoreApplication::quit();

            QTimer::singleShot(700, &app, []() {
                _exit(0);
            });
        }
    );

    qDebug() << "[Watcher] Single-model chat engine ready.";
    qDebug() << "[Watcher] Kind config entries:" << kindConfig.size();
    qDebug() << "[Watcher] Copilot / Ctrl+Alt+I: toggle chat UI";
    qDebug() << "[Watcher] Ctrl+C: copy selection";
    qDebug() << "[Watcher] Ctrl+Alt+Esc: quit";

    return app.exec();
}
