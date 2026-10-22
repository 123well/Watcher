#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>
#include <atomic>

#include "../model/flm_gateway.h"

class ChatEngine : public QAbstractListModel {
    Q_OBJECT

    Q_PROPERTY(bool thinking READ isThinking NOTIFY thinkingChanged)

public:
    enum Roles {
        RoleRole = Qt::UserRole + 1,
        ContentRole
    };

    struct Message {
        QString role;

        // Текст для UI.
        QString content;

        // Для tool / question.
        QString toolCallId;
        QString toolName;
        QString toolArguments;
        QString toolResult;
    };

    explicit ChatEngine(
        StreamQueue& queue,
        FlmGateway& gateway,
        QObject *parent = nullptr
    );

    ~ChatEngine() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isThinking() const;

    Q_INVOKABLE void resetSession();
    Q_INVOKABLE void sendMessage(const QString &text);
    Q_INVOKABLE void cancelActive();
    Q_INVOKABLE void copyToClipboard(const QString &text);

signals:
    void thinkingChanged(bool thinking);

private:
    struct ParsedToolCall {
        QString id;
        QString name;
        QString arguments;
        json args;
    };

    void workerLoop();
    void handleEvent(const StreamEvent &event);

    void sendRequest();

    json buildFlmMessages() const;
    json buildTools() const;

    ParsedToolCall parseToolCall(const QString &payload);
    QString executeTool(const ParsedToolCall &call) const;

    void insertMessage(const QString &role, const QString &content);
    void insertToolMessage(const ParsedToolCall &call, const QString &result);
    void insertQuestionMessage(const ParsedToolCall &call, const QString &question);
    bool answerPendingQuestion(const QString &answer);

    void appendOrUpdateLast(const QString &role, const QString &chunk);

    void setThinking(bool thinking);

    StreamQueue &m_queue;
    FlmGateway &m_gateway;

    QThread m_workerThread;

    std::atomic<bool> m_running{true};
    std::atomic<bool> m_thinking{false};

    QVector<Message> m_messages;

    QString m_currentRequestId;

    int m_toolIterations = 0;
    bool m_pendingToolContinuation = false;

    int m_submitRetries = 0;
    bool m_abortRequested = false;

    int m_toolCallCounter = 0;

    bool m_waitingForUser = false;
    int m_pendingQuestionIndex = -1;

    static constexpr int kMaxToolIterations = 4;
    static constexpr int kMaxSubmitRetries = 30;
};
