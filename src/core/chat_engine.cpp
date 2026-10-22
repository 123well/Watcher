#include "chat_engine.h"

#include <QMetaObject>
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include <QGuiApplication>
#include <QClipboard>

#include <chrono>
#include <string>

static const QString kSystemPrompt = QStringLiteral(
    "Ты — Watcher, локальный агент.\n"
    "Думай и используй инструменты, когда они нужны.\n"
    "Обычный текст assistant — это финальный ответ пользователю.\n"
    "Если тебе нужен ответ пользователя, вызови ask_user.\n"
    "Когда пользователь ответит, его ответ придёт как результат инструмента ask_user; продолжай тот же ход.\n"
    "Не выдумывай результаты инструментов.\n"
    "Отвечай кратко и по делу."
);

static const double kTemperature = 0.35;

ChatEngine::ChatEngine(
    StreamQueue &queue,
    FlmGateway &gateway,
    QObject *parent
)
    : QAbstractListModel(parent)
    , m_queue(queue)
    , m_gateway(gateway)
{
    qDebug() << "[ChatEngine] Created";

    Message system;
    system.role = QStringLiteral("system");
    system.content = kSystemPrompt;

    m_messages.push_back(system);

    connect(&m_workerThread, &QThread::started, this, [this]() {
        workerLoop();
    }, Qt::DirectConnection);

    m_workerThread.start();
}

ChatEngine::~ChatEngine()
{
    m_running = false;

    if (m_gateway.isBusy()) {
        m_gateway.cancelActive();
    }

    m_workerThread.wait();
}

int ChatEngine::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return m_messages.size();
}

QVariant ChatEngine::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size()) {
        return {};
    }

    const Message &message = m_messages[index.row()];

    switch (role) {
        case RoleRole:
            return message.role;

        case ContentRole:
            return message.content;

        default:
            return {};
    }
}

QHash<int, QByteArray> ChatEngine::roleNames() const
{
    return {
        {RoleRole, "role"},
        {ContentRole, "content"}
    };
}

bool ChatEngine::isThinking() const
{
    return m_thinking;
}

void ChatEngine::resetSession()
{
    if (m_thinking) {
        m_gateway.cancelActive();
        setThinking(false);
    }

    beginResetModel();

    m_messages.clear();

    Message system;
    system.role = QStringLiteral("system");
    system.content = kSystemPrompt;

    m_messages.push_back(system);

    m_currentRequestId.clear();

    m_toolIterations = 0;
    m_pendingToolContinuation = false;

    m_submitRetries = 0;
    m_abortRequested = false;

    m_toolCallCounter = 0;

    m_waitingForUser = false;
    m_pendingQuestionIndex = -1;

    endResetModel();
}

void ChatEngine::sendMessage(const QString &text)
{
    const QString trimmed = text.trimmed();

    if (trimmed.isEmpty()) {
        return;
    }

    if (m_waitingForUser) {
        if (answerPendingQuestion(trimmed)) {
            return;
        }

        m_waitingForUser = false;
        m_pendingQuestionIndex = -1;
    }

    if (m_thinking) {
        return;
    }

    insertMessage(QStringLiteral("user"), trimmed);

    m_toolIterations = 0;
    m_pendingToolContinuation = false;
    m_abortRequested = false;
    m_submitRetries = 0;

    setThinking(true);
    sendRequest();
}

void ChatEngine::cancelActive()
{
    m_abortRequested = true;

    if (m_waitingForUser) {
        m_waitingForUser = false;
        m_pendingQuestionIndex = -1;
    }

    if (m_thinking) {
        m_gateway.cancelActive();
        m_currentRequestId.clear();
        setThinking(false);
    } else {
        setThinking(false);
    }
}

void ChatEngine::copyToClipboard(const QString &text)
{
    if (auto *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
    }
}

void ChatEngine::workerLoop()
{
    while (m_running) {
        auto event = m_queue.pop_blocking(100);

        if (!event) {
            continue;
        }

        QMetaObject::invokeMethod(this, [this, ev = *event]() {
            handleEvent(ev);
        }, Qt::QueuedConnection);
    }
}

void ChatEngine::handleEvent(const StreamEvent &event)
{
    if (m_currentRequestId.isEmpty()) {
        return;
    }

    if (
        !event.requestId.empty() &&
        QString::fromStdString(event.requestId) != m_currentRequestId
    ) {
        return;
    }

    switch (event.type) {
        case StreamEvent::Type::Token: {
            const QString text = QString::fromStdString(event.text);
            appendOrUpdateLast(QStringLiteral("assistant"), text);
            break;
        }

        case StreamEvent::Type::ReasoningToken: {
            const QString text = QString::fromStdString(event.text);
            appendOrUpdateLast(QStringLiteral("reasoning"), text);
            break;
        }

        case StreamEvent::Type::ToolCall: {
            const QString payload = QString::fromStdString(event.payload);

            // FLM/Qwen иногда шлёт пустые пробельные content-токены перед tool_call.
            // Не тащим их ни в UI, ни в следующий промпт.
            if (
                !m_messages.isEmpty() &&
                m_messages.last().role == QStringLiteral("assistant") &&
                m_messages.last().content.trimmed().isEmpty()
            ) {
                const int row = m_messages.size() - 1;

                beginRemoveRows(QModelIndex(), row, row);
                m_messages.removeLast();
                endRemoveRows();
            }

            ParsedToolCall call = parseToolCall(payload);

            auto argString = [&](const char *key) -> QString {
                if (
                    call.args.contains(key) &&
                    call.args[key].is_string()
                ) {
                    return QString::fromStdString(
                        call.args[key].get<std::string>()
                    );
                }

                return QString();
            };

            // ask_user — терминальный интерактивный инструмент.
            if (call.name == QStringLiteral("ask_user")) {
                QString question = argString("question");

                if (question.trimmed().isEmpty()) {
                    question = argString("content");
                }

                if (question.trimmed().isEmpty()) {
                    question = payload;
                }

                insertQuestionMessage(call, question);

                m_pendingToolContinuation = false;
                m_currentRequestId.clear();
                m_submitRetries = 0;
                m_abortRequested = false;

                setThinking(false);
                break;
            }

            // Совместимость: если модель всё-таки вызывает answer_user,
            // превращаем это в обычный assistant-ответ.
            if (call.name == QStringLiteral("answer_user")) {
                QString content = argString("content");

                if (content.trimmed().isEmpty()) {
                    content = argString("answer");
                }

                if (content.trimmed().isEmpty()) {
                    content = QStringLiteral("Готово.");
                }

                insertMessage(QStringLiteral("assistant"), content);

                m_pendingToolContinuation = false;
                m_currentRequestId.clear();
                m_submitRetries = 0;
                m_abortRequested = false;

                setThinking(false);
                break;
            }

            const QString result = executeTool(call);

            insertToolMessage(call, result);

            ++m_toolIterations;

            m_pendingToolContinuation =
                m_toolIterations < kMaxToolIterations;

            break;
        }

        case StreamEvent::Type::ToolCallDelta:
        case StreamEvent::Type::Metrics:
            break;

        case StreamEvent::Type::Done: {
            if (m_pendingToolContinuation) {
                m_pendingToolContinuation = false;
                sendRequest();
                break;
            }

            m_currentRequestId.clear();
            m_submitRetries = 0;
            m_abortRequested = false;

            setThinking(false);
            break;
        }

        case StreamEvent::Type::Error:
        case StreamEvent::Type::Cancelled: {
            m_currentRequestId.clear();
            m_submitRetries = 0;
            m_abortRequested = false;

            setThinking(false);
            break;
        }

        default:
            break;
    }
}

void ChatEngine::sendRequest()
{
    if (m_abortRequested) {
        m_currentRequestId.clear();
        m_submitRetries = 0;
        setThinking(false);
        return;
    }

    LlmRequest request;

    request.id = "req-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );

    request.messages = buildFlmMessages();
    request.tools = buildTools();
    request.temperature = kTemperature;

    m_currentRequestId = QString::fromStdString(request.id);

    if (!m_gateway.trySubmit(request)) {
        if (m_submitRetries < kMaxSubmitRetries) {
            ++m_submitRetries;

            QTimer::singleShot(50, this, [this]() {
                sendRequest();
            });

            return;
        }

        m_currentRequestId.clear();
        m_submitRetries = 0;
        m_abortRequested = false;

        setThinking(false);
        return;
    }

    m_submitRetries = 0;
    setThinking(true);
}

json ChatEngine::buildFlmMessages() const
{
    json out = json::array();

    const int n = m_messages.size();

    auto isReasoning = [](const Message &m) {
        return m.role == QStringLiteral("reasoning");
    };

    auto isToolLike = [](const Message &m) {
        return
            m.role == QStringLiteral("tool") ||
            m.role == QStringLiteral("question");
    };

    auto usableTool = [&](const Message &m) {
        return isToolLike(m) && !m.toolResult.trimmed().isEmpty();
    };

    auto addToolPair = [](
        const Message &m,
        json &toolCalls,
        json &toolMessages,
        int fallbackIndex
    ) {
        QString id = m.toolCallId;

        if (id.isEmpty()) {
            id = QStringLiteral("call_%1").arg(fallbackIndex);
        }

        QString name = m.toolName;

        if (name.isEmpty()) {
            name = QStringLiteral("unknown");
        }

        QString arguments = m.toolArguments;

        if (arguments.isEmpty()) {
            arguments = QStringLiteral("{}");
        }

        json call;
        call["id"] = id.toStdString();
        call["type"] = "function";
        call["function"]["name"] = name.toStdString();
        call["function"]["arguments"] = arguments.toStdString();

        toolCalls.push_back(call);

        json toolMessage;
        toolMessage["role"] = "tool";
        toolMessage["tool_call_id"] = id.toStdString();
        toolMessage["content"] = m.toolResult.toStdString();

        toolMessages.push_back(toolMessage);
    };

    for (int i = 0; i < n; ) {
        const Message &msg = m_messages[i];

        if (isReasoning(msg)) {
            ++i;
            continue;
        }

        if (
            msg.role == QStringLiteral("system") ||
            msg.role == QStringLiteral("user")
        ) {
            out.push_back({
                {"role", msg.role.toStdString()},
                {"content", msg.content.toStdString()}
            });

            ++i;
            continue;
        }

        if (msg.role == QStringLiteral("assistant")) {
            int j = i + 1;

            while (
                j < n &&
                (
                    isReasoning(m_messages[j]) ||
                    (isToolLike(m_messages[j]) && !usableTool(m_messages[j]))
                )
            ) {
                ++j;
            }

            if (j < n && usableTool(m_messages[j])) {
                json toolCalls = json::array();
                json toolMessages = json::array();

                int k = j;
                int toolIndex = 1;

                while (k < n) {
                    const Message &candidate = m_messages[k];

                    if (isReasoning(candidate)) {
                        ++k;
                        continue;
                    }

                    if (!isToolLike(candidate)) {
                        break;
                    }

                    if (usableTool(candidate)) {
                        addToolPair(candidate, toolCalls, toolMessages, toolIndex++);
                    }

                    ++k;
                }

                json assistant;
                assistant["role"] = "assistant";

                if (msg.content.trimmed().isEmpty()) {
                    assistant["content"] = nullptr;
                } else {
                    assistant["content"] = msg.content.toStdString();
                }

                assistant["tool_calls"] = toolCalls;

                out.push_back(assistant);

                for (const auto &toolMessage : toolMessages) {
                    out.push_back(toolMessage);
                }

                i = k;
                continue;
            }

            out.push_back({
                {"role", "assistant"},
                {"content", msg.content.toStdString()}
            });

            ++i;
            continue;
        }

        if (usableTool(msg)) {
            json toolCalls = json::array();
            json toolMessages = json::array();

            int k = i;
            int toolIndex = 1;

            while (k < n) {
                const Message &candidate = m_messages[k];

                if (isReasoning(candidate)) {
                    ++k;
                    continue;
                }

                if (!isToolLike(candidate)) {
                    break;
                }

                if (usableTool(candidate)) {
                    addToolPair(candidate, toolCalls, toolMessages, toolIndex++);
                }

                ++k;
            }

            json assistant;
            assistant["role"] = "assistant";
            assistant["content"] = nullptr;
            assistant["tool_calls"] = toolCalls;

            out.push_back(assistant);

            for (const auto &toolMessage : toolMessages) {
                out.push_back(toolMessage);
            }

            i = k;
            continue;
        }

        // Неотвеченный question или неизвестная роль — в FLM не отправляем.
        ++i;
    }

    return out;
}

json ChatEngine::buildTools() const
{
    json tools = json::array();

    json getTime;
    getTime["type"] = "function";
    getTime["function"]["name"] = "get_time";
    getTime["function"]["description"] = "Вернуть текущие дату и время.";
    getTime["function"]["parameters"] = json::object();

    tools.push_back(getTime);

    json echo;
    echo["type"] = "function";
    echo["function"]["name"] = "echo";
    echo["function"]["description"] = "Вернуть переданный текст обратно.";
    echo["function"]["parameters"]["type"] = "object";
    echo["function"]["parameters"]["properties"]["text"]["type"] = "string";
    echo["function"]["parameters"]["properties"]["text"]["description"] =
        "Текст для повтора.";
    echo["function"]["parameters"]["required"] = json::array({"text"});

    tools.push_back(echo);

    json askUser;
    askUser["type"] = "function";
    askUser["function"]["name"] = "ask_user";
    askUser["function"]["description"] =
        "Задать пользователю вопрос и получить его ответ как результат инструмента. "
        "Используй только когда без ответа пользователя нельзя продолжить.";

    askUser["function"]["parameters"]["type"] = "object";
    askUser["function"]["parameters"]["properties"]["question"]["type"] = "string";
    askUser["function"]["parameters"]["properties"]["question"]["description"] =
        "Текст вопроса пользователю.";
    askUser["function"]["parameters"]["required"] = json::array({"question"});

    tools.push_back(askUser);

    return tools;
}

ChatEngine::ParsedToolCall ChatEngine::parseToolCall(const QString &payload)
{
    ParsedToolCall call;
    call.args = json::object();

    const std::string raw = payload.toStdString();
    json j = json::parse(raw, nullptr, false);

    auto readString = [](const json &value) -> QString {
        if (value.is_string()) {
            return QString::fromStdString(value.get<std::string>());
        }

        return QString();
    };

    auto applyArguments = [&](const json &value, ParsedToolCall &target) {
        if (value.is_string()) {
            target.arguments = QString::fromStdString(value.get<std::string>());
        } else if (!value.is_null()) {
            target.arguments = QString::fromStdString(value.dump());
            target.args = value;
        }
    };

    if (!j.is_discarded()) {
        if (j.contains("function") && j["function"].is_object()) {
            if (j.contains("id")) {
                call.id = readString(j["id"]);
            }

            const json &fn = j["function"];

            if (fn.contains("name")) {
                call.name = readString(fn["name"]);
            }

            if (fn.contains("arguments")) {
                applyArguments(fn["arguments"], call);
            }
        } else {
            if (j.contains("id")) {
                call.id = readString(j["id"]);
            } else if (j.contains("tool_call_id")) {
                call.id = readString(j["tool_call_id"]);
            }

            if (j.contains("name")) {
                call.name = readString(j["name"]);
            } else if (j.contains("tool")) {
                call.name = readString(j["tool"]);
            }

            if (j.contains("arguments")) {
                applyArguments(j["arguments"], call);
            } else if (j.contains("args")) {
                applyArguments(j["args"], call);
            } else if (j.contains("parameters")) {
                applyArguments(j["parameters"], call);
            }
        }
    }

    if (call.id.isEmpty()) {
        call.id = QStringLiteral("call_%1").arg(++m_toolCallCounter);
    }

    if (call.name.isEmpty()) {
        call.name = QStringLiteral("unknown");
    }

    if (call.arguments.isEmpty()) {
        call.arguments = QStringLiteral("{}");
    }

    json parsedArgs = json::parse(
        call.arguments.toStdString(),
        nullptr,
        false
    );

    if (!parsedArgs.is_discarded() && parsedArgs.is_object()) {
        call.args = parsedArgs;
    } else {
        call.args = json::object();
    }

    return call;
}

QString ChatEngine::executeTool(const ParsedToolCall &call) const
{
    if (call.name == QStringLiteral("get_time")) {
        return QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss")
        );
    }

    if (call.name == QStringLiteral("echo")) {
        if (
            call.args.contains("text") &&
            call.args["text"].is_string()
        ) {
            return QString::fromStdString(
                call.args["text"].get<std::string>()
            );
        }

        return QStringLiteral("Для echo ожидается строковый аргумент text.");
    }

    return QStringLiteral("Unknown tool: %1").arg(call.name);
}

void ChatEngine::insertMessage(const QString &role, const QString &content)
{
    Message message;
    message.role = role;
    message.content = content;

    const int row = m_messages.size();

    beginInsertRows(QModelIndex(), row, row);
    m_messages.push_back(message);
    endInsertRows();
}

void ChatEngine::insertToolMessage(
    const ParsedToolCall &call,
    const QString &result
)
{
    Message message;

    message.role = QStringLiteral("tool");
    message.toolCallId = call.id;
    message.toolName = call.name;
    message.toolArguments = call.arguments;
    message.toolResult = result;

    message.content = QStringLiteral("%1(%2)\n%3")
        .arg(call.name)
        .arg(call.arguments.isEmpty() ? QStringLiteral("{}") : call.arguments)
        .arg(result);

    const int row = m_messages.size();

    beginInsertRows(QModelIndex(), row, row);
    m_messages.push_back(message);
    endInsertRows();
}

void ChatEngine::insertQuestionMessage(
    const ParsedToolCall &call,
    const QString &question
)
{
    Message message;

    message.role = QStringLiteral("question");
    message.content = question;

    message.toolCallId = call.id;
    message.toolName = QStringLiteral("ask_user");
    message.toolArguments = call.arguments;
    message.toolResult = QString();

    const int row = m_messages.size();

    beginInsertRows(QModelIndex(), row, row);
    m_messages.push_back(message);
    endInsertRows();

    m_waitingForUser = true;
    m_pendingQuestionIndex = row;
}

bool ChatEngine::answerPendingQuestion(const QString &answer)
{
    int idx = m_pendingQuestionIndex;

    const auto isValidPending = [&](int i) {
        return
            i >= 0 &&
            i < m_messages.size() &&
            m_messages[i].role == QStringLiteral("question") &&
            m_messages[i].toolResult.trimmed().isEmpty();
    };

    if (!isValidPending(idx)) {
        idx = -1;

        for (int i = m_messages.size() - 1; i >= 0; --i) {
            if (isValidPending(i)) {
                idx = i;
                break;
            }
        }
    }

    if (idx < 0) {
        m_waitingForUser = false;
        m_pendingQuestionIndex = -1;
        return false;
    }

    Message &message = m_messages[idx];

    message.toolResult = answer;
    message.content += QStringLiteral("\n\nОтвет пользователя:\n") + answer;

    emit dataChanged(
        index(idx),
        index(idx),
        {ContentRole}
    );

    m_waitingForUser = false;
    m_pendingQuestionIndex = -1;

    m_pendingToolContinuation = false;
    m_abortRequested = false;
    m_submitRetries = 0;

    setThinking(true);
    sendRequest();

    return true;
}

void ChatEngine::appendOrUpdateLast(const QString &role, const QString &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }

    if (!m_messages.isEmpty()) {
        const Message &last = m_messages.last();

        if (last.role == role) {
            const int row = m_messages.size() - 1;

            m_messages[row].content += chunk;

            emit dataChanged(
                index(row),
                index(row),
                {ContentRole}
            );

            return;
        }
    }

    insertMessage(role, chunk);
}

void ChatEngine::setThinking(bool thinking)
{
    if (m_thinking == thinking) {
        return;
    }

    m_thinking = thinking;
    emit thinkingChanged(thinking);
}
