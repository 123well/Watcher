#include "global_input.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include <linux/input.h>

// Если в заголовках ядра ещё нет KEY_COPILOT, используем известное значение.
// 0x2e5 = 741 decimal.
#ifndef KEY_COPILOT
#define KEY_COPILOT 0x2e5
#endif

GlobalInput::GlobalInput(QObject *parent)
    : QObject(parent) {}

GlobalInput::~GlobalInput() {
    for (auto &dev : m_devices) {
        if (dev.notifier) {
            dev.notifier->setEnabled(false);
            delete dev.notifier;
        }
        if (dev.fd >= 0) {
            ::close(dev.fd);
        }
    }
}

bool GlobalInput::init() {
    setupDevices();
    return !m_devices.isEmpty();
}

static QString readDeviceName(const QString &eventName) {
    QString path = QStringLiteral("/sys/class/input/%1/device/name").arg(eventName);
    QFile file(path);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString name = QString::fromUtf8(file.readAll()).trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }

    path = QStringLiteral("/sys/class/input/%1/device/device/name").arg(eventName);
    file.setFileName(path);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString name = QString::fromUtf8(file.readAll()).trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }

    return eventName;
}

void GlobalInput::setupDevices() {
    QDir inputDir("/dev/input");
    QStringList entries = inputDir.entryList({"event*"}, QDir::System, QDir::Name);

    qDebug() << "[GlobalInput] Found" << entries.size() << "input devices";

    for (const QString &name : entries) {
        QString path = "/dev/input/" + name;
        int fd = ::open(path.toUtf8().constData(), O_RDONLY | O_NONBLOCK);

        if (fd < 0) {
            qDebug() << "[GlobalInput] Cannot open" << path << ":" << strerror(errno);
            continue;
        }

        QString deviceName = readDeviceName(name);

        unsigned long evBits[(EV_MAX + 7) / 8] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) < 0) {
            qDebug() << "[GlobalInput] Cannot get EV bits for" << deviceName << path;
            ::close(fd);
            continue;
        }

        auto testBit = [](unsigned long *bits, int bit) {
            return (bits[bit / 8] >> (bit % 8)) & 1;
        };

        if (!testBit(evBits, EV_KEY)) {
            ::close(fd);
            continue;
        }

        unsigned long keyBits[(KEY_MAX + 7) / 8] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0) {
            qDebug() << "[GlobalInput] Cannot get KEY bits for" << deviceName << path;
            ::close(fd);
            continue;
        }

        bool hasQ = testBit(keyBits, KEY_Q);
        bool hasA = testBit(keyBits, KEY_A);
        bool hasZ = testBit(keyBits, KEY_Z);

        bool hasKeyboardLetters = hasQ || hasA || hasZ;

        bool hasMouseButtons =
            testBit(keyBits, BTN_LEFT) ||
            testBit(keyBits, BTN_RIGHT) ||
            testBit(keyBits, BTN_MIDDLE);

        bool hasRelativeMotion = false;

        if (testBit(evBits, EV_REL)) {
            unsigned long relBits[(REL_MAX + 7) / 8] = {0};

            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits) >= 0) {
                hasRelativeMotion =
                    testBit(relBits, REL_X) ||
                    testBit(relBits, REL_Y);
            }
        }

        if (hasKeyboardLetters && !hasMouseButtons && !hasRelativeMotion) {
            qDebug() << "[GlobalInput] Accepted keyboard device:" << deviceName << path;

            KeyboardDevice dev;
            dev.fd = fd;
            dev.notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);

            connect(dev.notifier, &QSocketNotifier::activated, this, [this, fd]() {
                handleKeyboardEvent(fd);
            });

            m_devices.append(dev);
        } else {
            ::close(fd);
        }
    }

    if (m_devices.isEmpty()) {
        qWarning() << "[GlobalInput] No keyboard devices found.";
        qWarning() << "[GlobalInput] Check group:";
        qWarning() << "[GlobalInput]   groups | grep input";
    }
}

void GlobalInput::handleKeyboardEvent(int fd) {
    struct input_event ev;

    while (true) {
        ssize_t n = ::read(fd, &ev, sizeof(ev));

        if (n != sizeof(ev)) {
            break;
        }

        if (ev.type != EV_KEY) {
            continue;
        }

        const int code = static_cast<int>(ev.code);
        const int value = static_cast<int>(ev.value);

        // Shift
        if (code == KEY_LEFTSHIFT) {
            if (value == 1) m_leftShift = true;
            else if (value == 0) m_leftShift = false;
            continue;
        }

        if (code == KEY_RIGHTSHIFT) {
            if (value == 1) m_rightShift = true;
            else if (value == 0) m_rightShift = false;
            continue;
        }

        // Ctrl
        if (code == KEY_LEFTCTRL) {
            if (value == 1) m_leftCtrl = true;
            else if (value == 0) m_leftCtrl = false;
            continue;
        }

        if (code == KEY_RIGHTCTRL) {
            if (value == 1) m_rightCtrl = true;
            else if (value == 0) m_rightCtrl = false;
            continue;
        }

        // Alt
        if (code == KEY_LEFTALT) {
            if (value == 1) m_leftAlt = true;
            else if (value == 0) m_leftAlt = false;
            continue;
        }

        if (code == KEY_RIGHTALT) {
            if (value == 1) m_rightAlt = true;
            else if (value == 0) m_rightAlt = false;
            continue;
        }

        // Meta / Win
        if (code == KEY_LEFTMETA) {
            if (value == 1) m_leftMeta = true;
            else if (value == 0) m_leftMeta = false;
            continue;
        }

        if (code == KEY_RIGHTMETA) {
            if (value == 1) m_rightMeta = true;
            else if (value == 0) m_rightMeta = false;
            continue;
        }

        // Игнорируем отпускание и автоповтор обычных клавиш.
        if (value != 1) {
            continue;
        }

        const bool shiftPressed = m_leftShift || m_rightShift;
        const bool ctrlPressed = m_leftCtrl || m_rightCtrl;
        const bool altPressed = m_leftAlt || m_rightAlt;
        const bool metaPressed = m_leftMeta || m_rightMeta;

        // Copilot: открыть/закрыть UI.
        if (code == KEY_COPILOT) {
            emit hotkeyToggle();
            continue;
        }

        // Запасной вариант для Copilot: Win + Shift + F23.
        if (code == KEY_F23 && shiftPressed && metaPressed) {
            emit hotkeyToggle();
            continue;
        }

        // Ctrl + Alt + I: тоже открыть/закрыть UI.
        if (code == KEY_I && ctrlPressed && altPressed) {
            emit hotkeyInput();
            continue;
        }

        // Ctrl + C: глобальное копирование выделения из оверлея.
        // Alt и Meta не должны быть нажаты, чтобы не ловить системные комбинации.
        if (code == KEY_C && ctrlPressed && !altPressed && !metaPressed) {
            qDebug() << "[GlobalInput] Copy hotkey detected";
            emit copyRequested();
            continue;
        }

        // Ctrl + Alt + Esc: завершить Watcher.
        if (code == KEY_ESC && ctrlPressed && altPressed) {
            qDebug() << "[GlobalInput] Quit hotkey detected";
            emit quitRequested();
            continue;
        }

        // Игнорируем мышь/тачпад/планшетные коды.
        if (code >= 0x100) {
            continue;
        }

        emit keyPressed(code);
    }
}
