#pragma once

#include "rag_types.h"
#include "searcher.h"
#include "store.h"
#include "vector_index.h"

#include <QObject>

#include <memory>

namespace rag {

class EmbeddingProvider;

// Фасад. CLI и будущий ChatEngine говорят только с ним.
// Ошибки возвращаются в структурах результата и НИКОГДА не летят
// отдельным сигналом в UI — UI не должен знать про сбои индекса.
class RagEngine : public QObject {
    Q_OBJECT

public:
    explicit RagEngine(QObject *parent = nullptr);
    ~RagEngine() override;

    bool init(const QString &dbPath, EmbeddingProvider *provider, QString *err);

    // === три операции ===
    InsertResult insert_data(
        const QString &title,
        const QString &text,
        SourceType sourceType,
        const QString &sourceUri,
        const QStringList &tags,
        const QJsonObject &metadata
    );

    DeleteResult delete_data(qint64 docId);
    DeleteResult delete_data_by_uri(const QString &sourceUri);

    SearchOutcome rag_search(const SearchQuery &query);

    // === служебное, не для модели ===
    bool document(qint64 id, DocRecord *out);
    StatsInfo stats();
    QVector<DocRecord> list_docs(int offset, int limit, int *total);
    int chunk_count(qint64 docId);
    bool update_tags(qint64 id, const QStringList &tags);
    bool reindex(QString *err);

    EmbeddingProvider *provider() const { return m_provider.get(); }

signals:
    // Только наблюдаемость: прогресс и факты. Не ошибки для пользователя.
    void modelLoaded(const QString &modelId);
    void modelUnloaded();
    void embeddingComputed(int texts, qint64 ms);
    void documentInserted(qint64 docId, int chunks, bool unchanged);
    void documentDeleted(qint64 docId, int chunks);
    void searchPerformed(int results, qint64 ms);
    void indexChanged(int chunks, long long bytes);

private:
    std::vector<std::vector<float>> embedBatch(
        const std::vector<std::string> &texts, qint64 *ms, QString *err);

    std::unique_ptr<RagStore> m_store;
    std::unique_ptr<VectorIndex> m_index;
    std::unique_ptr<EmbeddingProvider> m_provider;
    std::unique_ptr<RagSearcher> m_searcher;
};

} // namespace rag