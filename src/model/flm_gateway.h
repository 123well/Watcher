#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <optional>
#include <deque>
#include <condition_variable>
#include <unordered_map>

using json = nlohmann::json;

struct StreamEvent {
    enum class Type { 
        Token,
        ReasoningToken,
        ToolCall,
        ToolCallDelta,
        Done, 
        Error, 
        Cancelled, 
        Metrics
    };
    Type type;
    std::string requestId;
    std::string text;
    std::string payload;
};

struct LlmRequest {
    std::string id;
    json messages;
    json response_format;
    json tools;
    double temperature = 0.7;
};

class StreamQueue {
public:
    void push(StreamEvent event);
    std::optional<StreamEvent> pop_blocking(int timeout_ms = 100);
private:
    std::deque<StreamEvent> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

class FlmGateway {
public:
    FlmGateway(StreamQueue& queue, const std::string& host, int port);
    ~FlmGateway();

    bool trySubmit(const LlmRequest& request);
    void cancelActive();
    bool isBusy() const;

private:
    void workerMain(LlmRequest request);
    void processSseEvent(const std::string& event_str, const std::string& reqId, 
                         std::unordered_map<int, json>& tool_calls_acc);
    void pushEvent(StreamEvent::Type type, const std::string& reqId, 
                   const std::string& text, const std::string& payload);
    void finishActive();

    StreamQueue& m_queue;
    std::string m_host;
    int m_port;

    mutable std::mutex m_mutex;
    bool m_busy = false;
    std::string m_activeRequestId;
    std::atomic<bool> m_cancelRequested{false};
    std::thread m_worker;
};