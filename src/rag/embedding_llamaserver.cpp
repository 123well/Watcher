#include "embedding_llamaserver.h"
#include "rag_util.h"

#include <nlohmann/json.hpp>

#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace rag {

namespace {

using json = nlohmann::json;

constexpr int kPostTimeoutMs  = 300000;  // длинный ingest батчем
constexpr int kReadyDeadlineMs = 180000; // холодная загрузка + упаковка весов

int pickFreePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }

    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }

    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;   // гонка окна мала на localhost; см. --port override
}

void setSocketTimeouts(int fd, int ms)
{
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

std::string toLowerAscii(std::string s)
{
    for (char &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string dechunk(const std::string &in)
{
    std::string out;
    size_t i = 0;

    while (i < in.size()) {
        const size_t eol = in.find("\r\n", i);
        if (eol == std::string::npos) break;

        std::string szhex = in.substr(i, eol - i);
        const size_t sc = szhex.find(';');          // strip chunk-ext
        if (sc != std::string::npos) szhex = szhex.substr(0, sc);

        const unsigned long sz = std::strtoul(szhex.c_str(), nullptr, 16);
        i = eol + 2;

        if (sz == 0) break;
        if (i + sz > in.size()) { out.append(in.substr(i)); break; }

        out.append(in.substr(i, sz));
        i += sz;

        if (i + 1 < in.size() && in[i] == '\r' && in[i + 1] == '\n') i += 2;
    }

    return out;
}

struct HttpResponse {
    int status = -1;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// Один HTTP/1.1 запрос к 127.0.0.1:port, чтение до EOF (Connection: close).
bool httpRequest(int port, const std::string &method, const std::string &path,
                 const std::string &body, int timeoutMs,
                 HttpResponse *out, std::string *err)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { if (err) *err = std::strerror(errno); return false; }

    setSocketTimeouts(fd, timeoutMs);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(uint16_t(port));

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = std::strerror(errno);
        ::close(fd);
        return false;
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n"
                    + "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                    + "Connection: close\r\n";
    if (!body.empty()) {
        req += std::string("Content-Type: application/json\r\n")
             + "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) { if (err) *err = std::strerror(errno); ::close(fd); return false; }
        sent += size_t(n);
    }

    std::string raw;
    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n > 0) { raw.append(buf, size_t(n)); continue; }
        break;   // 0 = EOF, <0 = таймаут (для GET /health это «ещё не готов»)
    }
    ::close(fd);

    if (raw.empty()) { if (err) *err = "no response"; return false; }

    const size_t sep = raw.find("\r\n\r\n");
    const std::string head = (sep == std::string::npos) ? raw : raw.substr(0, sep);
    std::string bodyOut = (sep == std::string::npos) ? std::string() : raw.substr(sep + 4);

    {   // статус из первой строки: "HTTP/1.1 200 OK"
        const size_t sp1 = head.find(' ');
        const size_t sp2 = head.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos)
            out->status = std::atoi(head.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
    }

    if (toLowerAscii(head).find("transfer-encoding: chunked") != std::string::npos)
        bodyOut = dechunk(bodyOut);

    out->body = std::move(bodyOut);
    return true;
}

const char *poolingArg(Pooling p)
{
    switch (p) {
        case Pooling::Mean: return "mean";
        case Pooling::Last: return "last";
        default:            return "cls";
    }
}

} // namespace

LlamaServerEmbeddingProvider::LlamaServerEmbeddingProvider(
    QString serverBin, QString modelPath, QString modelId,
    int maxInputTokens, int threads, int httpPort, Pooling pooling)
    : m_serverBin(std::move(serverBin))
    , m_modelPath(std::move(modelPath))
    , m_modelId(std::move(modelId))
    , m_maxInputTokens(maxInputTokens > 0 ? maxInputTokens : 8192)
    , m_threads(threads)
    , m_requestedPort(httpPort)
    , m_pooling(pooling)
{
}

LlamaServerEmbeddingProvider::~LlamaServerEmbeddingProvider()
{
    unload();
}

bool LlamaServerEmbeddingProvider::isLoaded() const
{
    return m_proc.state() == QProcess::Running;
}

bool LlamaServerEmbeddingProvider::startServer(QString *err)
{
    if (isLoaded()) return true;

    if (!QFileInfo::exists(m_serverBin)) {
        if (err) *err = QStringLiteral("llama-server не найден: %1 "
                     "(укажи --llama-server, $WATCHER_LLAMA_SERVER или положи в PATH)")
                     .arg(m_serverBin);
        return false;
    }
    if (!QFileInfo::exists(m_modelPath)) {
        if (err) *err = QStringLiteral("GGUF не найден: %1").arg(m_modelPath);
        return false;
    }

    m_port = (m_requestedPort > 0) ? m_requestedPort : pickFreePort();
    if (m_port <= 0) {
        if (err) *err = QStringLiteral("не удалось подобрать свободный порт");
        return false;
    }

    QStringList args;
    args << QStringLiteral("-m") << m_modelPath
         << QStringLiteral("--embedding")
         << QStringLiteral("--pooling") << QString::fromLatin1(poolingArg(m_pooling))
         << QStringLiteral("-c") << QString::number(m_maxInputTokens)
         << QStringLiteral("--host") << QStringLiteral("127.0.0.1")
         << QStringLiteral("--port") << QString::number(m_port);
    if (m_threads > 0) args << QStringLiteral("-t") << QString::number(m_threads);

    m_proc.setProgram(m_serverBin);
    m_proc.setArguments(args);
    m_proc.setProcessChannelMode(QProcess::MergedChannels);  // stdout+stderr в один буфер
    m_proc.start();

    if (!m_proc.waitForStarted(10000)) {
        if (err) *err = QStringLiteral("llama-server не стартовал: %1")
                            .arg(m_proc.errorString());
        return false;
    }

    return true;
}

bool LlamaServerEmbeddingProvider::waitReady(int timeoutMs, QString *err)
{
    QElapsedTimer et;
    et.start();

    while (et.elapsed() < timeoutMs) {
        if (m_proc.state() != QProcess::Running) {
            m_logTail = QString::fromUtf8(m_proc.readAll()).right(4096);
            if (err) *err = QStringLiteral(
                "llama-server завершился при старте (exit %1).\n%2")
                .arg(m_proc.exitCode()).arg(m_logTail);
            return false;
        }

        HttpResponse hr;
        std::string e;
        // Готов = /health отвечает 200. 503 = ещё грузится, спать.
        if (httpRequest(m_port, "GET", "/health", "", 2000, &hr, &e) && hr.status == 200)
            return true;

        QThread::msleep(250);
    }

    m_logTail = QString::fromUtf8(m_proc.readAll()).right(4096);
    if (err) *err = QStringLiteral("таймаут готовности llama-server (%1 мс).\n%2")
                        .arg(timeoutMs).arg(m_logTail);
    return false;
}

bool LlamaServerEmbeddingProvider::ensureLoaded(QString *err)
{
    if (isLoaded()) {
        HttpResponse hr; std::string e;
        if (httpRequest(m_port, "GET", "/health", "", 2000, &hr, &e) && hr.status == 200)
            return true;
        unload();   // мог упасть между вызовами — переподнимаем
    }

    if (!startServer(err)) return false;
    return waitReady(kReadyDeadlineMs, err);
}

void LlamaServerEmbeddingProvider::unload()
{
    if (m_proc.state() == QProcess::NotRunning) return;

    m_proc.terminate();
    if (!m_proc.waitForFinished(3000)) {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }
    m_proc.readAll();   // вычищаем буфер, чтобы хвост не рос
}

std::vector<std::vector<float>> LlamaServerEmbeddingProvider::postBatch(
    const std::vector<std::string> &slice, QString *err)
{
    json input = json::array();
    for (const std::string &s : slice) input.push_back(s);

    json body;
    body["model"] = m_modelId.toStdString();
    body["input"] = input;

    HttpResponse hr;
    std::string e;
    if (!httpRequest(m_port, "POST", "/v1/embeddings", body.dump(), kPostTimeoutMs, &hr, &e)) {
        if (err) *err = QStringLiteral("HTTP POST не прошёл: %1")
                            .arg(QString::fromStdString(e));
        return {};
    }
    if (!hr.ok()) {
        if (err) *err = QStringLiteral("llama-server HTTP %1: %2")
                            .arg(hr.status)
                            .arg(QString::fromStdString(hr.body).left(400));
        return {};
    }

    json j = json::parse(hr.body, nullptr, false);
    if (j.is_discarded() || !j.contains("data")) {
        if (err) *err = QStringLiteral("непонятный ответ /v1/embeddings: %1")
                            .arg(QString::fromStdString(hr.body).left(300));
        return {};
    }

    std::vector<std::vector<float>> out;
    out.reserve(j["data"].size());

    for (const auto &item : j["data"]) {
        std::vector<float> v;
        for (const auto &x : item["embedding"]) v.push_back(float(x.get<double>()));
        if (m_dimensions <= 0) m_dimensions = int(v.size());
        out.push_back(normalizeVector(std::move(v)));
    }

    return out;
}

std::vector<std::vector<float>> LlamaServerEmbeddingProvider::embed(
    const std::vector<std::string> &texts, QString *err)
{
    if (texts.empty()) return {};
    if (!ensureLoaded(err)) return {};

    std::vector<std::vector<float>> out;
    out.reserve(texts.size());

    const size_t kBatch = 32;
    for (size_t i = 0; i < texts.size(); i += kBatch) {
        const std::vector<std::string> slice(
            texts.begin() + i,
            texts.begin() + std::min(texts.size(), i + kBatch));

        auto part = postBatch(slice, err);
        if (part.empty()) return {};
        out.insert(out.end(),
                   std::make_move_iterator(part.begin()),
                   std::make_move_iterator(part.end()));
    }

    return out;
}

} // namespace rag
