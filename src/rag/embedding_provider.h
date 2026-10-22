#pragma once

#include <QString>

#include <vector>

namespace rag {

// Пулинг per-token эмбеддингов в один вектор. Мы пулим ВРУЧНУЮ, не полагаясь
// на pooling_type в GGUF — иначе качество падает тихо (см. embedding_llama).
enum class Pooling { Cls, Mean, Last };

Pooling poolingFromString(const QString &s);
QString poolingToString(Pooling p);

class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;

    virtual QString name() const = 0;
    virtual QString modelId() const = 0;
    virtual int dimensions() const = 0;
    virtual int maxInputTokens() const = 0;
    virtual Pooling pooling() const = 0;

    // Модель грузится лениво. Вызывающий решает, когда держать загруженной.
    virtual bool ensureLoaded(QString *err) = 0;
    virtual void unload() = 0;
    virtual bool isLoaded() const = 0;

    // Батч строк -> батч НОРМАЛИЗОВАННЫХ векторов, порядок сохранён.
    // Пустой вектор при ошибке + заполненная err.
    virtual std::vector<std::vector<float>> embed(
        const std::vector<std::string> &texts,
        QString *err
    ) = 0;
};

} // namespace rag