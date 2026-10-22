#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QVector>

class GlobalInput : public QObject {
    Q_OBJECT

public:
    explicit GlobalInput(QObject *parent = nullptr);
    ~GlobalInput();

    bool init();

signals:
    void keyPressed(int scanCode);
    void hotkeyToggle();
    void hotkeyInput();
    void copyRequested();
    void quitRequested();

private:
    struct KeyboardDevice {
        int fd = -1;
        QSocketNotifier *notifier = nullptr;
    };

    void setupDevices();
    void handleKeyboardEvent(int fd);

    QVector<KeyboardDevice> m_devices;

    bool m_leftShift = false;
    bool m_rightShift = false;

    bool m_leftCtrl = false;
    bool m_rightCtrl = false;

    bool m_leftAlt = false;
    bool m_rightAlt = false;

    bool m_leftMeta = false;
    bool m_rightMeta = false;
};