#include "rag_engine.h"
#include "chunker.h"
#include "embedding_provider.h"
#include "rag_util.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>

namespace rag {

QString sourceToString(SourceType t)
{
    switch (t) {
        case SourceType::Docs:        return QStringLiteral("docs");
        case SourceType::Notes:       return QStringLiteral("notes");
        case SourceType::Code:        return QStringLiteral("code");
        case SourceType::ChatHistory: return QStringLiteral("chat_history");
        case SourceType::WebClip:     return QStringLiteral("web_clip");
        case SourceType::Fact:        return QStringLiteral("fact");
        default:                      return QStringLiteral("unknown");
    }
}

SourceType sourceFromString(const QString &s)
{
    if (s == QLatin1String("docs"))         return SourceType::Docs;
    if (s == QLatin1String("notes"))        return SourceType::Notes;
    if (s == QLatin1String("code"))         return SourceType::Code;
    if (s == QLatin1String("chat_history")) return SourceType::ChatHistory;
    if (s == QLatin1String("web_clip"))     return SourceType::WebClip;
    if (s == QLatin1String("fact"))         return SourceType::Fact;
    return SourceType::Unknown;
}

double sourceTrust(SourceType t)
{
    switch (t) {
        case SourceType::Fact:        return 1.00;
        case SourceType::Docs:        return 1.00;
        case SourceType::Notes:       return 0.97;
        case SourceType::Code:        return 0.95;
        case SourceType::ChatHistory: return 0.85;
        case SourceType::WebClip:     return 0.75;
        default:                      return 0.90;
    }
}

RagEngine::RagEngine(QObject *parent) : QObject(parent) {}
RagEngine::~RagEngine() = default;

bool RagEngine::init(const QString &dbPath, EmbeddingProvider *provider, QString *err)
{
    m_provider.reset(provider);
    m_store = std::make_unique<RagStore>();
    m_index = std::make_unique<VectorIndex>();

    if (!m_store->open(dbPath, err)) {
        return false;
    }

    m_store->setDimensions(m_provider->dimensions());

    m_searcher = std::make_unique<RagSearcher>(
        m_store.get(), m_index.get(), m_provider.get());

    return true;
}

// Батч с одним load/unload на вызов — ровно схема: подняли, всё посчитали, сбросили.
std::vector<std::vector<float>> RagEngine::embedBatch(
    const std::vector<std::string> &texts, qint64 *ms, QString *err)
{
    QElapsedTimer t;
    t.start();

    if (!m_provider->ensureLoaded(err)) {
        emit modelUnloaded();
        return {};
    }

    emit modelLoaded(m_provider->modelId());

    auto out = m_provider->embed(texts, err);

    m_provider->unload();
    emit modelUnloaded();

    if (ms) *ms = t.elapsed();
    if (!out.empty()) emit embeddingComputed(int(out.size()), t.elapsed());

    return out;
}

InsertResult RagEngine::insert_data(
    const QString &title,
    const QString &text,
    SourceType sourceType,
    const QString &sourceUri,
    const QStringList &tags,
    const QJsonObject &metadata)
{
    InsertResult r;

    QElapsedTimer t;
    t.start();

    if (text.trimmed().isEmpty()) {
        r.error = QStringLiteral("empty text");
        return r;
    }

    const QString norm = normalizeForHash(text);
    const QString hash = sha256Hex(norm.toUtf8());

    // Дешёвый дедуп по содержимому: тот же текст — тот же документ.
    const qint64 existing = m_store->findDocByHash(hash);

    if (existing > 0) {
        r.ok = true;
        r.docId = existing;
        r.unchanged = true;
        r.chunks = m_store->countChunks(existing);
        r.totalMs = t.elapsed();
        r.warnings.push_back(QStringLiteral("дубликат по hash, пропущено"));

        emit documentInserted(existing, r.chunks, true);
        return r;
    }

    const ChunkPlan plan = chunkDocument(sourceType, sourceUri, text);
    const SourceType effType = plan.detected;

    // Готовим ВСЁ одним батчем: [0]=title, [1]=doc-представление, [2..]=чанки.
    std::vector<std::string> batch;
    batch.reserve(size_t(plan.chunks.size()) + 2);

    const QString tTitle = title.trimmed();

    // Представление документа: title + содержание. bge-m3 принимает 8192 токена,
    // поэтому «вектор по всему содержанию» реально работает для статей.
    QString docRepr = tTitle + QStringLiteral("\n\n") + norm;

    const int docTokens = estimateTokens(docRepr);
    const int maxTok = m_provider->maxInputTokens();

    bool docTrunc = false;

    if (docTokens > maxTok) {
        const int keepChars = qMax(1000, maxTok * 35 / 10);
        docRepr = docRepr.left(keepChars);
        docTrunc = true;
    }

    batch.push_back(tTitle.toUtf8().constData());
    batch.push_back(docRepr.toUtf8().constData());

    for (const ChunkDraft &c : plan.chunks) {
        // Чанк эмбедим с меткой: label несёт навигационный сигнал, почти бесплатен.
        const QString repr = c.label.isEmpty()
            ? c.text
            : c.label + QStringLiteral("\n") + c.text;

        batch.push_back(repr.toUtf8().constData());
    }

    qint64 embedMs = 0;
    QString err;

    const auto vectors = embedBatch(batch, &embedMs, &err);

    if (vectors.empty()) {
        r.error = QStringLiteral("embedding failed: %1").arg(err);
        return r;
    }

    if (int(vectors.size()) != int(batch.size())) {
        r.error = QStringLiteral("embedding count mismatch: wanted %1 got %2")
                      .arg(batch.size()).arg(vectors.size());
        return r;
    }

    // --- документ ---
    DocRecord d;
    d.sourceType = effType;
    d.sourceUri = sourceUri;
    d.title = tTitle.isEmpty() ? guessTitle(effType, sourceUri, text) : tTitle;
    d.text = text;
    d.contentHash = hash;
    d.tags = tags;
    d.metadata = metadata;
    d.metadata.insert(QStringLiteral("embedding_model"), m_provider->modelId());
    d.docVector = vectors[1];
    d.docVectorTruncated = docTrunc;
    d.docVectorCoveredTokens = docTrunc ? maxTok : docTokens;
    d.titleVector = vectors[0];

    const qint64 docId = m_store->insertDocument(d, &err);

    if (docId < 0) {
        r.error = QStringLiteral("insert document: %1").arg(err);
        return r;
    }

    // --- чанки ---
    QVector<ChunkRecord> chunks;
    chunks.reserve(plan.chunks.size());

    for (int i = 0; i < plan.chunks.size(); ++i) {
        const ChunkDraft &cd = plan.chunks[i];

        ChunkRecord c;
        c.docId = docId;
        c.ordinal = i;
        c.text = cd.text;
        c.label = cd.label;
        c.tokenEst = cd.tokenEst > 0 ? cd.tokenEst : estimateTokens(cd.text);
        c.charStart = cd.charStart;
        c.charEnd = cd.charEnd;
        c.metadata = cd.metadata;
        c.metadata.insert(QStringLiteral("embedding_model"), m_provider->modelId());
        if (!tags.isEmpty()) {
            c.metadata.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
        }
        c.vector = vectors[size_t(i) + 2];

        chunks.push_back(c);
    }

    const int inserted = m_store->insertChunks(docId, chunks, &err);

    if (inserted < 0) {
        r.error = QStringLiteral("insert chunks: %1").arg(err);
        return r;
    }

    // Реальные id чанков в индекс — только если индекс уже загружен.
    if (m_searcher->indexLoaded()) {
        const QVector<qint64> ids = m_store->chunkIdsOfDoc(docId, inserted);

        for (int i = 0; i < ids.size() && i < chunks.size(); ++i) {
            m_index->add(ids[i], chunks[i].vector);
        }
    }
    // Если индекс ещё не загружен — не трогаем: первый rag_search
    // построит его из БД, где новый документ уже лежит.

    r.ok = true;
    r.docId = docId;
    r.chunks = inserted;
    r.docVectorTruncated = docTrunc;
    r.embedMs = embedMs;
    r.totalMs = t.elapsed();
    r.warnings = plan.warnings;

    if (docTrunc) {
        r.warnings.push_back(QStringLiteral(
            "doc-вектор усечён: %1 токенов > окно %2 — чанк-вектора "
            "несут основную работу поиска").arg(docTokens).arg(maxTok));
    }

    emit documentInserted(docId, inserted, false);
    emit indexChanged(m_store->stats().chunks, m_index->bytes());

    return r;
}

DeleteResult RagEngine::delete_data(qint64 docId)
{
    auto r = m_store->deleteDocument(docId);

    if (r.ok) {
        m_searcher->invalidateIndex();
        emit documentDeleted(docId, r.chunks);
    }

    return r;
}

DeleteResult RagEngine::delete_data_by_uri(const QString &sourceUri)
{
    const qint64 id = m_store->findDocByUri(sourceUri);

    if (id < 0) {
        DeleteResult r;
        r.error = QStringLiteral("not found: %1").arg(sourceUri);
        return r;
    }

    return delete_data(id);
}

SearchOutcome RagEngine::rag_search(const SearchQuery &query)
{
    SearchOutcome r = m_searcher->search(query);

    if (r.ok) {
        emit searchPerformed(int(r.snippets.size()), r.totalMs);
    }

    return r;
}

bool RagEngine::document(qint64 id, DocRecord *out)
{
    return m_store->getDocument(id, out);
}

StatsInfo RagEngine::stats()
{
    StatsInfo s = m_store->stats();

    s.indexChunks = m_index->size();
    s.embeddingModel = m_provider->modelId();
    s.provider = m_provider->name();
    s.pooling = poolingToString(m_provider->pooling());

    if (s.dimensions == 0) {
        s.dimensions = m_provider->dimensions();
    }

    return s;
}

// Полная пересборка векторов: нужен при смене embedding-модели —
// размерности/семантика несовместимы. До смены модели не вызывается.
bool RagEngine::reindex(QString *err)
{
    const auto ids = m_store->allChunkIds();
    if (ids.isEmpty()) return true;

    QVector<ChunkRecord> chunks;
    m_store->getChunksByIds(ids, &chunks);

    const int batch = 32;
    const QString model = m_provider->modelId();
    const int dim = m_provider->dimensions();

    for (int i = 0; i < chunks.size(); i += batch) {
        std::vector<std::string> texts;
        const int end = qMin(chunks.size(), i + batch);

        for (int j = i; j < end; ++j) {
            const QString repr = chunks[j].label.isEmpty()
                ? chunks[j].text
                : chunks[j].label + QStringLiteral("\n") + chunks[j].text;
            texts.push_back(repr.toUtf8().constData());
        }

        qint64 ms = 0;
        const auto v = embedBatch(texts, &ms, err);

        if (v.empty()) return false;

        for (int j = 0; j < int(v.size()); ++j) {
            const qint64 cid = chunks[size_t(i) + size_t(j)].id;
            if (!m_store->updateChunkVector(cid, v[size_t(j)], model, dim)) {
                if (err) *err = QStringLiteral("updateChunkVector failed id=%1").arg(cid);
                return false;
            }
        }
    }

    m_searcher->invalidateIndex();
    return true;
}

QVector<DocRecord> RagEngine::list_docs(int offset, int limit, int *total)
{
    return m_store->listDocs(offset, limit, total);
}

int RagEngine::chunk_count(qint64 docId)
{
    return m_store->countChunks(docId);
}

bool RagEngine::update_tags(qint64 id, const QStringList &tags)
{
    return m_store->updateDocumentTags(id, tags);
}

} // namespace rag