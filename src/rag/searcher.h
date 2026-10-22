#pragma once

#include "rag_types.h"
#include "store.h"
#include "vector_index.h"

namespace rag {

class EmbeddingProvider;

class RagSearcher {
public:
    RagSearcher(RagStore *store, VectorIndex *index, EmbeddingProvider *provider);

    SearchOutcome search(const SearchQuery &q);

    // Индекс ленивый: грузим вектора при первом поиске, а не при старте.
    // m_loadedOnce защищает инкрементальные add от перетирания загрузкой.
    void ensureIndex();
    void invalidateIndex();
    bool indexLoaded() const { return m_loadedOnce; }

    void setRecencyTauDays(double d) { m_recencyTau = d; }

private:
    QString buildFtsQuery(const QString &raw) const;
    void applyFilters(QVector<Candidate> *cands, const SearchQuery &q);
    QVector<Snippet> materialize(
        const QVector<Candidate> &ranked, const SearchQuery &q, int *dropped);

    RagStore *m_store;
    VectorIndex *m_index;
    EmbeddingProvider *m_provider;

    bool m_loadedOnce = false;
    double m_recencyTau = 45.0;
};

} // namespace rag