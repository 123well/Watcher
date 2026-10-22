#pragma once

#include "embedding_provider.h"

#include <QProcess>
#include <QString>

namespace rag {

// llama.cpp КАК БИНАРНИК, а не как библиотека: поднимаем llama-server
// ребёнком на время батча, считаем /v1/embeddings по localhost, убиваем.
// Ноль компилируемого чужого кода => GCC16/unity/per-arch не наша проблема.
// Качество bge-m3 сохраняется: токенизатор и пулинг делает сам llama-server
// (--pooling cls), не рукописный код.
//
// Схема load->compute->unload = spawn->POST->kill. Между вызовами RAM чиста.
class LlamaServerEmbeddingProvider : public EmbeddingProvider {
public:
    LlamaServerEmbeddingProvider(
        QString serverBin, QString modelPath, QString modelId,
        int maxInputTokens, int threads, int httpPort, Pooling pooling);

    ~LlamaServerEmbeddingProvider() override;

    QString name() const override { return QStringLiteral("llama-server"); }
    QString modelId() const override { return m_modelId; }
    int dimensions() const override { return m_dimensions; }   // 0 до первого embed
    int maxInputTokens() const override { return m_maxInputTokens; }
    Pooling pooling() const override { return m_pooling; }
    bool isLoaded() const override;

    bool ensureLoaded(QString *err) override;
    void unload() override;

    std::vector<std::vector<float>> embed(
        const std::vector<std::string> &texts, QString *err) override;

    QString serverLogTail() const { return m_logTail; }

private:
    bool startServer(QString *err);
    bool waitReady(int timeoutMs, QString *err);
    std::vector<std::vector<float>> postBatch(
        const std::vector<std::string> &slice, QString *err);

    QString m_serverBin, m_modelPath, m_modelId;
    int m_maxInputTokens = 8192;
    int m_threads = 0;
    int m_requestedPort = 0;
    int m_port = 0;
    int m_dimensions = 0;
    Pooling m_pooling = Pooling::Cls;

    QProcess m_proc;
    QString m_logTail;
};

} // namespace rag
