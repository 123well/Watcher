#include "console_agent.h"
#include <iostream>
#include <chrono>

ConsoleAgent::ConsoleAgent(FlmGateway& gateway, StreamQueue& queue)
    : m_gateway(gateway), m_queue(queue)
{
    m_history.push_back({
        {"role", "system"},
        {"content", "Ты - полезный ассистент. Отвечай кратко и по делу."}
    });
}

ConsoleAgent::~ConsoleAgent() {
    stop();
}

void ConsoleAgent::run() {
    printUsage();

    m_inputThread = std::thread(&ConsoleAgent::inputLoop, this);
    m_eventThread = std::thread(&ConsoleAgent::eventLoop, this);

    m_inputThread.join();
    m_eventThread.join();
}

void ConsoleAgent::stop() {
    m_running = false;
    m_gateway.cancelActive();
    
    if (m_inputThread.joinable()) m_inputThread.join();
    if (m_eventThread.joinable()) m_eventThread.join();
}

void ConsoleAgent::inputLoop() {
    std::string line;
    while (m_running) {
        std::cout << "\n\033[1;32mYou > \033[0m";
        if (!std::getline(std::cin, line)) {
            m_running = false;
            break;
        }

        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        if (line.empty()) continue;

        if (line == "/quit" || line == "/exit") {
            m_running = false;
            break;
        } else if (line == "/cancel") {
            m_gateway.cancelActive();
            std::cout << "[ConsoleAgent] Request cancelled.\n";
        } else if (line == "/history") {
            std::cout << "\n--- History (" << m_history.size() << " messages) ---\n";
            for (const auto& msg : m_history) {
                std::string content = msg.value("content", "");
                if (content.length() > 100) content = content.substr(0, 100) + "...";
                std::cout << msg["role"].get<std::string>() << ": " << content << "\n";
            }
        } else if (line == "/clear") {
            m_history.clear();
            m_history.push_back({
                {"role", "system"},
                {"content", "Ты - полезный ассистент. Отвечай кратко и по делу."}
            });
            std::cout << "[ConsoleAgent] History cleared.\n";
        } else if (line == "/help") {
            printUsage();
        } else {
            sendUserMessage(line);
        }
    }
}

void ConsoleAgent::sendUserMessage(const std::string& text) {
    if (m_gateway.isBusy()) {
        std::cout << "\n[ConsoleAgent] FLM is busy, please wait...\n";
        return;
    }

    m_history.push_back({
        {"role", "user"},
        {"content", text}
    });

    LlmRequest req;
    req.id = "req-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    req.messages = m_history;
    req.temperature = 0.7;

    m_currentResponse.clear();

    if (!m_gateway.trySubmit(req)) {
        std::cout << "\n[ConsoleAgent] Failed to submit request.\n";
        m_history.pop_back();
    }
}

void ConsoleAgent::eventLoop() {
    while (m_running) {
        auto ev = m_queue.pop_blocking(100);
        if (!ev) continue;

        handleEvent(*ev);
    }
}

void ConsoleAgent::handleEvent(const StreamEvent& event) {
    switch (event.type) {
        case StreamEvent::Type::ReasoningToken:
            std::cout << "\033[90m" << event.text << "\033[0m" << std::flush;
            break;

        case StreamEvent::Type::Token:
            m_currentResponse += event.text;
            std::cout << event.text << std::flush;
            break;

        case StreamEvent::Type::ToolCall:
            std::cout << "\n\033[92m[TOOL CALL] " << event.payload << "\033[0m\n";
            break;

        case StreamEvent::Type::ToolCallDelta:
            break;

        case StreamEvent::Type::Metrics:
            std::cout << "\n\033[94m[METRICS] " << event.payload << "\033[0m\n";
            break;

        case StreamEvent::Type::Done:
            std::cout << "\n";
            if (!m_currentResponse.empty()) {
                m_history.push_back({
                    {"role", "assistant"},
                    {"content", m_currentResponse}
                });
            }
            m_currentResponse.clear();
            break;

        case StreamEvent::Type::Error:
            std::cout << "\n\033[31m[ERROR] " << event.payload << "\033[0m\n";
            m_currentResponse.clear();
            break;

        case StreamEvent::Type::Cancelled:
            std::cout << "\n\033[33m[CANCELLED]\033[0m\n";
            if (!m_currentResponse.empty()) {
                m_history.push_back({
                    {"role", "assistant"},
                    {"content", m_currentResponse}
                });
            }
            m_currentResponse.clear();
            break;
    }
}

void ConsoleAgent::printUsage() {
    std::cout << "\n\033[1;36m=== FLM Console Agent ===\033[0m\n";
    std::cout << "Commands:\n";
    std::cout << "  /help    - show this help\n";
    std::cout << "  /cancel  - cancel current request\n";
    std::cout << "  /history - show message history\n";
    std::cout << "  /clear   - clear history\n";
    std::cout << "  /quit    - exit\n";
    std::cout << "\033[1;36m=========================\033[0m\n";
}