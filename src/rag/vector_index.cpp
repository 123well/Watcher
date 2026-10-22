#include "vector_index.h"

#include <algorithm>

namespace rag {

void VectorIndex::build(const QVector<qint64> &ids, const std::vector<float> &flat, int dim)
{
    m_ids = ids;
    m_flat = flat;
    m_dim = dim;
    m_count = dim > 0 ? int(flat.size() / size_t(dim)) : 0;
}

void VectorIndex::add(qint64 id, const std::vector<float> &v)
{
    if (v.empty()) return;

    if (m_dim == 0) m_dim = int(v.size());
    if (int(v.size()) != m_dim) return;

    m_ids.push_back(id);
    m_flat.insert(m_flat.end(), v.begin(), v.end());
    ++m_count;
}

void VectorIndex::remove(qint64 id)
{
    const int pos = m_ids.indexOf(id);
    if (pos < 0) return;

    m_ids.remove(pos);

    const auto off = m_flat.begin() + pos * ptrdiff_t(m_dim);
    m_flat.erase(off, off + m_dim);

    --m_count;
}

void VectorIndex::clear()
{
    m_ids.clear();
    m_flat.clear();
    m_count = 0;
}

long long VectorIndex::bytes() const
{
    return static_cast<long long>(m_count) * static_cast<long long>(m_dim) * 4LL;
}

QVector<std::pair<qint64, float>> VectorIndex::search(
    const std::vector<float> &query, int k) const
{
    QVector<std::pair<qint64, float>> out;

    if (m_count == 0 || m_dim == 0 || int(query.size()) != m_dim || k <= 0) {
        return out;
    }

    out.reserve(m_count);

    const float *rows = m_flat.data();

    for (int i = 0; i < m_count; ++i) {
        const float *r = rows + size_t(i) * size_t(m_dim);

        double sum = 0.0;

        // Векторизуется компилятором (AVX2/AVX-512) — причина, почему
        // точный обход здесь дёшев.
        for (int d = 0; d < m_dim; ++d) {
            sum += double(r[d]) * double(query[size_t(d)]);
        }

        out.push_back({m_ids[i], float(sum)});
    }

    std::partial_sort(
        out.begin(),
        out.begin() + qMin(k, out.size()),
        out.end(),
        [](const auto &a, const auto &b) { return a.second > b.second; });

    out.resize(qMin(k, out.size()));

    return out;
}

} // namespace rag