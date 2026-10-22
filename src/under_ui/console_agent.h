#pragma once

#include "../model/flm_gateway.h"
#include <vector>
#include <atomic>

class ConsoleAgent {
public:
    ConsoleAgent(FlmGateway& gateway, StreamQueue& queue);
    ~ConsoleAgent();

    void run();
    void stop();

private:
    void inputLoop();
    void eventLoop();
    void sendUserMessage(const std::string& text);
    void handleEvent(const StreamEvent& event);
    void printUsage();

    FlmGateway& m_gateway;
    StreamQueue& m_queue;

    std::vector<json> m_history;
    std::string m_currentResponse;
    std::atomic<bool> m_running{true};
    std::thread m_inputThread;
    std::thread m_eventThread;
};