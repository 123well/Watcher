#pragma once

#include <QVector>

#include <utility>
#include <vector>

namespace rag {

class VectorIndex {
public:
    // Возвращает (chunkId, dot) — векторы нормализованы, dot == косинус.
    QVector<std::pair<qint64, float>> search(const std::vector<float> &query, int k) const;

    void build(const QVector<qint64> &ids, const std::vector<float> &flat, int dim);
    void add(qint64 id, const std::vector<float> &v);
    void remove(qint64 id);
    void clear();

    int size() const { return m_count; }
    int dim() const { return m_dim; }
    bool loaded() const { return m_count > 0; }

    long long bytes() const;   // count * dim * 4

private:
    QVector<qint64> m_ids;
    std::vector<float> m_flat;
    int m_dim = 0;
    int m_count = 0;
};

} // namespace rag