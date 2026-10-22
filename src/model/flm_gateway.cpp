#include "flm_gateway.h"
#include <iostream>

// --- StreamQueue ---

void StreamQueue::push(StreamEvent event) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(event));
    m_cv.notify_one();
}

std::optional<StreamEvent> StreamQueue::pop_blocking(int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]{ return !m_queue.empty(); })) {
        StreamEvent event = std::move(m_queue.front());
        m_queue.pop_front();
        return event;
    }
    return std::nullopt;
}

// --- FlmGateway ---

FlmGateway::FlmGateway(StreamQueue& queue, const std::string& host, int port)
    : m_queue(queue), m_host(host), m_port(port) {}

FlmGateway::~FlmGateway() {
    cancelActive();
    if (m_worker.joinable()) m_worker.join();
}

bool FlmGateway::trySubmit(const LlmRequest& request) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_busy) {
        std::cout << "[FlmGateway] FLM is busy. Request " << request.id << " ignored.\n";
        return false;
    }

    m_busy = true;
    m_activeRequestId = request.id;
    m_cancelRequested = false;

    if (m_worker.joinable()) m_worker.join();

    m_worker = std::thread(&FlmGateway::workerMain, this, request);
    return true;
}

void FlmGateway::cancelActive() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_busy) return;
    m_cancelRequested = true;
}

bool FlmGateway::isBusy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_busy;
}

void FlmGateway::workerMain(LlmRequest request) {
    httplib::Client client(m_host, m_port);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(300, 0);
    client.set_keep_alive(true);
    
    json body;
    body["model"] = "qwen3:8b";
    body["messages"] = request.messages;
    body["stream"] = true;
    body["temperature"] = request.temperature;
    
    if (!request.response_format.is_null()) body["response_format"] = request.response_format;
    if (!request.tools.is_null()) body["tools"] = request.tools;

    std::string sse_buffer;
    std::unordered_map<int, json> tool_calls_accumulator;

    httplib::ContentReceiver receiver = [&](const char *data, size_t data_len) -> bool {
        if (m_cancelRequested) {
            return false;
        }

        sse_buffer.append(data, data_len);
        size_t pos;
        
        while ((pos = sse_buffer.find("\n\n")) != std::string::npos) {
            std::string event_str = sse_buffer.substr(0, pos);
            sse_buffer.erase(0, pos + 2);

            processSseEvent(event_str, request.id, tool_calls_accumulator);
        }
        return true;
    };

    httplib::Headers headers = {
        {"Accept", "text/event-stream"},
        {"Cache-Control", "no-cache"}
    };
    
    auto res = client.Post(
        "/v1/chat/completions", 
        headers, 
        body.dump(), 
        "application/json", 
        receiver, 
        nullptr
    );

    const bool wasCancelled = m_cancelRequested.load();

    finishActive();

    if (wasCancelled) {
        pushEvent(StreamEvent::Type::Cancelled, request.id, "", "");
    } else if (!res) {
        pushEvent(StreamEvent::Type::Error, request.id, "", httplib::to_string(res.error()));
    } else {
        if (res->status != 200) {
            pushEvent(StreamEvent::Type::Error, request.id, "", res->body);
        }

        pushEvent(StreamEvent::Type::Done, request.id, "", "");
    }
}

void FlmGateway::processSseEvent(const std::string& event_str, const std::string& reqId, 
                                 std::unordered_map<int, json>& tool_calls_acc) {
    if (event_str.rfind("data: ", 0) != 0) return;

    std::string json_str = event_str.substr(6);
    if (json_str == "[DONE]") return;

    try {
        json j = json::parse(json_str);
        
        if (j.contains("choices") && !j["choices"].empty()) {
            auto& choice = j["choices"][0];
            auto& delta = choice["delta"];
            
            // Reasoning (скрытые мысли)
            if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null()) {
                pushEvent(StreamEvent::Type::ReasoningToken, reqId, 
                         delta["reasoning_content"].get<std::string>(), "");
            } 
            // Финальный контент
            else if (delta.contains("content") && !delta["content"].is_null()) {
                pushEvent(StreamEvent::Type::Token, reqId, 
                         delta["content"].get<std::string>(), "");
            }
            
            // Tool Calls (инкрементальная сборка)
            if (delta.contains("tool_calls") && !delta["tool_calls"].is_null()) {
                auto& tool_calls = delta["tool_calls"];
                
                for (auto& tc : tool_calls) {
                    int index = tc["index"].get<int>();
                    
                    if (tool_calls_acc.find(index) == tool_calls_acc.end()) {
                        tool_calls_acc[index] = {
                            {"index", index},
                            {"id", tc.value("id", "")},
                            {"type", "function"},
                            {"function", {
                                {"name", ""},
                                {"arguments", ""}
                            }}
                        };
                    }
                    
                    auto& acc = tool_calls_acc[index];
                    
                    if (tc.contains("function") && tc["function"].contains("name")) {
                        acc["function"]["name"] = acc["function"]["name"].get<std::string>() + 
                                                 tc["function"]["name"].get<std::string>();
                    }
                    
                    if (tc.contains("function") && tc["function"].contains("arguments")) {
                        acc["function"]["arguments"] = acc["function"]["arguments"].get<std::string>() + 
                                                       tc["function"]["arguments"].get<std::string>();
                    }
                    
                    pushEvent(StreamEvent::Type::ToolCallDelta, reqId, "", tc.dump());
                }
            }
            
            // finish_reason
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                std::string reason = choice["finish_reason"].get<std::string>();
                
                if (reason == "tool_calls") {
                    for (auto& [idx, tc] : tool_calls_acc) {
                        pushEvent(StreamEvent::Type::ToolCall, reqId, "", tc.dump());
                    }
                    tool_calls_acc.clear();
                }
            }
        }

        // Метрики
        if (j.contains("usage") && !j["usage"].is_null()) {
            pushEvent(StreamEvent::Type::Metrics, reqId, "", j["usage"].dump());
        }

    } catch (const std::exception& e) {
        std::cerr << "[FlmGateway] JSON parse error: " << e.what() << "\n";
    }
}

void FlmGateway::pushEvent(StreamEvent::Type type, const std::string& reqId, 
                           const std::string& text, const std::string& payload) {
    StreamEvent ev;
    ev.type = type;
    ev.requestId = reqId;
    ev.text = text;
    ev.payload = payload;
    m_queue.push(std::move(ev));
}

void FlmGateway::finishActive() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_busy = false;
    m_activeRequestId.clear();
}