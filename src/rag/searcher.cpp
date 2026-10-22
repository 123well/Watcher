#include "searcher.h"
#include "embedding_provider.h"
#include "rag_util.h"

#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace rag {

namespace {

constexpr double kRrfK = 60.0;

double rrf(int rank)
{
    if (rank < 0) return 0.0;
    return 1.0 / (kRrfK + double(rank) + 1.0);
}

} // namespace

RagSearcher::RagSearcher(RagStore *store, VectorIndex *index, EmbeddingProvider *provider)
    : m_store(store), m_index(index), m_provider(provider)
{
}

void RagSearcher::ensureIndex()
{
    if (m_loadedOnce) return;

    QVector<qint64> ids;
    std::vector<float> flat;
    int dim = 0;

    m_store->loadChunkVectors(&ids, &flat, &dim);
    m_index->build(ids, flat, dim);

    m_loadedOnce = true;
}

void RagSearcher::invalidateIndex()
{
    m_loadedOnce = false;
    m_index->clear();
}

QString RagSearcher::buildFtsQuery(const QString &raw) const
{
    static const QRegularExpression nonWord(QStringLiteral("[^\\p{L}\\p{N}_+]+"));

    // Служебные слова. Без фильтра «для/что/как» попадает в каждый документ,
    // BM25 рапортует bm25:N при нулевой дискриминации и может перевернуть
    // ранжирование: док с cos 0.30, содержащий «для», получает +1/61 и бьёт
    // док с cos 0.60, который «для» не содержит.
    static const QSet<QString> stop = {
        // ru
        QStringLiteral("что"), QStringLiteral("как"), QStringLiteral("для"),
        QStringLiteral("из"), QStringLiteral("по"), QStringLiteral("к"),
        QStringLiteral("на"), QStringLiteral("в"), QStringLiteral("во"),
        QStringLiteral("с"), QStringLiteral("со"), QStringLiteral("и"),
        QStringLiteral("а"), QStringLiteral("но"), QStringLiteral("или"),
        QStringLiteral("либо"), QStringLiteral("это"), QStringLiteral("этот"),
        QStringLiteral("эта"), QStringLiteral("эти"), QStringLiteral("его"),
        QStringLiteral("её"), QStringLiteral("их"), QStringLiteral("не"),
        QStringLiteral("ни"), QStringLiteral("да"), QStringLiteral("нет"),
        QStringLiteral("быть"), QStringLiteral("был"), QStringLiteral("была"),
        QStringLiteral("есть"), QStringLiteral("будет"), QStringLiteral("будут"),
        QStringLiteral("использовать"), QStringLiteral("можно"),
        QStringLiteral("нужно"), QStringLiteral("надо"), QStringLiteral("чем"),
        QStringLiteral("кто"), QStringLiteral("где"), QStringLiteral("когда"),
        QStringLiteral("почему"), QStringLiteral("зачем"), QStringLiteral("какой"),
        QStringLiteral("какая"), QStringLiteral("какое"), QStringLiteral("какие"),
        QStringLiteral("мой"), QStringLiteral("твой"), QStringLiteral("наш"),
        QStringLiteral("ваш"), QStringLiteral("себя"), QStringLiteral("себе"),
        QStringLiteral("о"), QStringLiteral("об"), QStringLiteral("от"),
        QStringLiteral("до"), QStringLiteral("за"), QStringLiteral("под"),
        QStringLiteral("над"), QStringLiteral("между"), QStringLiteral("через"),
        QStringLiteral("про"), QStringLiteral("при"), QStringLiteral("без"),
        QStringLiteral("кроме"), QStringLiteral("после"), QStringLiteral("перед"),
        QStringLiteral("всего"), QStringLiteral("более"), QStringLiteral("менее"),
        QStringLiteral("очень"), QStringLiteral("так"), QStringLiteral("такой"),
        QStringLiteral("такая"), QStringLiteral("такое"), QStringLiteral("такие"),
        QStringLiteral("же"), QStringLiteral("ли"), QStringLiteral("бы"),
        // en
        QStringLiteral("the"), QStringLiteral("a"), QStringLiteral("an"),
        QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("but"),
        QStringLiteral("of"), QStringLiteral("to"), QStringLiteral("in"),
        QStringLiteral("on"), QStringLiteral("at"), QStringLiteral("by"),
        QStringLiteral("for"), QStringLiteral("with"), QStringLiteral("from"),
        QStringLiteral("is"), QStringLiteral("are"), QStringLiteral("was"),
        QStringLiteral("were"), QStringLiteral("be"), QStringLiteral("been"),
        QStringLiteral("it"), QStringLiteral("its"), QStringLiteral("this"),
        QStringLiteral("that"), QStringLiteral("these"), QStringLiteral("those"),
        QStringLiteral("as"), QStringLiteral("if"), QStringLiteral("then"),
        QStringLiteral("than"), QStringLiteral("so"), QStringLiteral("not"),
        QStringLiteral("do"), QStringLiteral("does"), QStringLiteral("did"),
        QStringLiteral("can"), QStringLiteral("could"), QStringLiteral("should"),
        QStringLiteral("would"), QStringLiteral("will"), QStringLiteral("shall"),
        QStringLiteral("may"), QStringLiteral("might"), QStringLiteral("must"),
        QStringLiteral("have"), QStringLiteral("has"), QStringLiteral("had"),
        QStringLiteral("what"), QStringLiteral("which"), QStringLiteral("who"),
        QStringLiteral("whom"), QStringLiteral("when"), QStringLiteral("where"),
        QStringLiteral("why"), QStringLiteral("how"), QStringLiteral("all"),
        QStringLiteral("any"), QStringLiteral("both"), QStringLiteral("each"),
        QStringLiteral("few"), QStringLiteral("more"), QStringLiteral("most"),
        QStringLiteral("other"), QStringLiteral("some"), QStringLiteral("such"),
        QStringLiteral("only"), QStringLiteral("own"), QStringLiteral("same"),
        QStringLiteral("too"), QStringLiteral("very"), QStringLiteral("just"),
        QStringLiteral("also"), QStringLiteral("into"), QStringLiteral("over"),
        QStringLiteral("under"), QStringLiteral("again"), QStringLiteral("once"),
        QStringLiteral("here"), QStringLiteral("there"), QStringLiteral("about"),
        QStringLiteral("against"), QStringLiteral("during"), QStringLiteral("before"),
        QStringLiteral("after"), QStringLiteral("above"), QStringLiteral("below"),
        QStringLiteral("up"), QStringLiteral("down"), QStringLiteral("out"),
        QStringLiteral("off"), QStringLiteral("through"), QStringLiteral("while"),
    };

    QString tmp = raw;
    tmp.replace(nonWord, QStringLiteral(" "));
    const QStringList tokens = tmp.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QStringList quoted;
    for (const QString &t : tokens) {
        if (t.size() < 2) continue;
        if (stop.contains(t.toLower())) continue;   // <-- фикс утечки
        quoted.push_back(QLatin1Char('"') + t + QLatin1Char('"'));
    }

    if (quoted.isEmpty()) return {};

    quoted = QStringList(quoted.begin(), quoted.begin() + qMin(12, quoted.size()));
    return quoted.join(QStringLiteral(" OR "));
}

void RagSearcher::applyFilters(QVector<Candidate> *cands, const SearchQuery &q)
{
    if (q.sources.isEmpty() && q.tags.isEmpty()) return;

    const QSet<QString> srcSet(q.sources.begin(), q.sources.end());
    const QSet<QString> tagSet(q.tags.begin(), q.tags.end());

    QHash<qint64, DocRecord> byId;
    for (const Candidate &c : *cands) {
        if (byId.contains(c.docId)) continue;
        DocRecord d;
        if (m_store->getDocument(c.docId, &d)) byId.insert(c.docId, d);
    }

    QVector<Candidate> kept;
    for (const Candidate &c : *cands) {
        const DocRecord d = byId.value(c.docId);

        if (!srcSet.isEmpty() && !srcSet.contains(sourceToString(d.sourceType))) continue;

        if (!tagSet.isEmpty()) {
            bool hit = false;
            for (const QString &t : d.tags) {
                if (tagSet.contains(t)) { hit = true; break; }
            }
            if (!hit) continue;
        }

        kept.push_back(c);
    }

    *cands = kept;
}

QVector<Snippet> RagSearcher::materialize(
    const QVector<Candidate> &ranked, const SearchQuery &q, int *dropped)
{
    QVector<Snippet> out;
    if (ranked.isEmpty()) return out;

    QVector<qint64> chunkIds;
    for (const Candidate &c : ranked) chunkIds.push_back(c.chunkId);

    QVector<ChunkRecord> chunks;
    m_store->getChunksByIds(chunkIds, &chunks);

    QHash<qint64, ChunkRecord> chunkById;
    for (const ChunkRecord &c : chunks) chunkById.insert(c.id, c);

    QHash<qint64, DocRecord> docById;
    for (const Candidate &c : ranked) {
        if (docById.contains(c.docId)) continue;
        DocRecord d;
        if (m_store->getDocument(c.docId, &d)) docById.insert(c.docId, d);
    }

    int budget = q.tokenBudget;
    int rank = 0;

    for (const Candidate &c : ranked) {
        const ChunkRecord ch = chunkById.value(c.chunkId);
        if (ch.text.isEmpty()) continue;

        const int cost = qMax(1, ch.tokenEst);

        if (!out.isEmpty() && cost > budget) {
            if (dropped) ++(*dropped);
            continue;
        }
        budget -= cost;

        Snippet s;
        s.rank = ++rank;
        s.chunkId = ch.id;
        s.docId = ch.docId;
        s.label = ch.label;
        s.tokenEst = ch.tokenEst;
        s.score = c.finalScore;
        s.vectorScore = c.vectorScore;
        s.vectorRank = c.vectorRank;
        s.lexicalRank = c.lexicalRank;
        s.createdAt = ch.createdAt;
        s.metadata = ch.metadata;

        const DocRecord d = docById.value(ch.docId);
        s.title = d.title;
        s.sourceType = sourceToString(d.sourceType);
        s.sourceUri = d.sourceUri;

        bool tr = false;
        s.text = truncateUtf8(ch.text, q.maxSnippetChars, &tr);
        s.truncated = tr;

        out.push_back(s);
        if (int(out.size()) >= q.limit) break;
    }

    return out;
}

SearchOutcome RagSearcher::search(const SearchQuery &query)
{
    SearchOutcome r;
    QElapsedTimer t; t.start();

    if (query.query.trimmed().isEmpty()) {
        r.error = QStringLiteral("empty query");
        return r;
    }

    ensureIndex();

    QElapsedTimer te; te.start();
    QString err;

    if (!m_provider->ensureLoaded(&err)) {
        r.error = QStringLiteral("embedding load failed: %1").arg(err);
        return r;
    }

    const auto vecs = m_provider->embed({query.query.toUtf8().constData()}, &err);
    r.embedMs = te.elapsed();

    if (vecs.empty()) {
        r.error = QStringLiteral("embedding failed: %1").arg(err);
        return r;
    }

    m_provider->unload();

    const std::vector<float> &qv = vecs.front();

    if (m_index->dim() > 0 && int(qv.size()) != m_index->dim()) {
        r.error = QStringLiteral(
            "dimension mismatch: index=%1 query=%2 — сменилась embedding-модель, нужен rebuild")
            .arg(m_index->dim()).arg(int(qv.size()));
        return r;
    }

    const auto vhits = m_index->search(qv, query.candidatePool);
    r.indexedChunks = m_index->size();
    r.vectorCandidates = vhits.size();

    QHash<qint64, Candidate> pool;
    for (int i = 0; i < vhits.size(); ++i) {
        Candidate c;
        c.chunkId = vhits[i].first;
        c.vectorRank = i;
        c.vectorScore = vhits[i].second;
        c.fusedScore = float(rrf(i));
        pool.insert(c.chunkId, c);
    }

    const QString ftsQ = buildFtsQuery(query.query);
    if (!ftsQ.isEmpty()) {
        const auto lhits = m_store->ftsSearch(ftsQ, query.candidatePool);
        r.lexicalCandidates = lhits.size();

        for (int i = 0; i < lhits.size(); ++i) {
            const qint64 id = lhits[i].first;
            Candidate &c = pool[id];
            if (c.chunkId < 0) c.chunkId = id;
            c.lexicalRank = i;
            c.fusedScore += float(rrf(i));
        }
    }

    QVector<qint64> ids;
    for (auto it = pool.begin(); it != pool.end(); ++it) ids.push_back(it.key());

    QVector<ChunkRecord> chunks;
    m_store->getChunksByIds(ids, &chunks);
    for (const ChunkRecord &c : chunks)
        if (pool.contains(c.id)) pool[c.id].docId = c.docId;

    QVector<Candidate> cands;
    for (auto it = pool.begin(); it != pool.end(); ++it)
        if (it.value().docId > 0) cands.push_back(it.value());

    applyFilters(&cands, query);

    QHash<qint64, DocRecord> docById;
    for (const Candidate &c : cands) {
        if (docById.contains(c.docId)) continue;
        DocRecord d;
        if (m_store->getDocument(c.docId, &d)) docById.insert(c.docId, d);
    }

    for (Candidate &c : cands) {
        const DocRecord d = docById.value(c.docId);
        const double trust = sourceTrust(d.sourceType);
        double recency = 1.0;
        if (query.recencyBoost) {
            const double age = daysSince(d.createdAt);
            recency = 0.85 + 0.30 * std::exp(-age / m_recencyTau);
        }
        c.trust = float(trust);
        c.recency = float(recency);
        c.finalScore = c.fusedScore;   // RRF — первичный сигнал релевантности
    }

    // trust/recency — ТОЛЬКО tie-breaker при равном RRF. Мультипликативный
    // trust перебивал ранг: полоса RRF сужается с ростом ранга (~1.6% между
    // rank0/1, <1% глубоко), и любой множитель вроде 0.97 переворачивал порядок
    // документа с лучшим косинусом только из-за метки источника.
    std::sort(cands.begin(), cands.end(),
        [](const Candidate &a, const Candidate &b) {
            if (std::fabs(a.fusedScore - b.fusedScore) > 1e-9f)
                return a.fusedScore > b.fusedScore;
            if (std::fabs(a.trust - b.trust) > 1e-9f)
                return a.trust > b.trust;
            return a.recency > b.recency;
        });

    r.snippets = materialize(cands, query, &r.droppedByBudget);
    r.searchMs = t.elapsed();
    r.totalMs = r.searchMs;
    r.ok = true;

    if (r.snippets.isEmpty() && r.indexedChunks == 0)
        r.warnings.push_back(QStringLiteral("индекс пуст — ничего не проиндексировано"));

    return r;
}

} // namespace rag
