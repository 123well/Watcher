#include "store.h"
#include "rag_util.h"

#include <sqlite3.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstring>

namespace rag {

namespace {

const char *kSchema = R"SQL(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS documents (
    id                     INTEGER PRIMARY KEY AUTOINCREMENT,
    source_type            TEXT NOT NULL,
    source_uri             TEXT,
    title                  TEXT NOT NULL,
    text                   TEXT NOT NULL,
    content_hash           TEXT NOT NULL,
    metadata_json          TEXT,
    tags                   TEXT,
    doc_vector             BLOB,
    doc_vector_truncated   INTEGER NOT NULL DEFAULT 0,
    doc_vector_covered     INTEGER NOT NULL DEFAULT 0,
    title_vector           BLOB,
    embedding_model        TEXT,
    dimensions             INTEGER,
    created_at             TEXT NOT NULL,
    updated_at             TEXT NOT NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_doc_hash ON documents(content_hash);
CREATE INDEX IF NOT EXISTS idx_doc_uri ON documents(source_uri);
CREATE INDEX IF NOT EXISTS idx_doc_type ON documents(source_type);

CREATE TABLE IF NOT EXISTS chunks (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    document_id     INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
    ordinal         INTEGER NOT NULL,
    text            TEXT NOT NULL,
    label           TEXT,
    token_est       INTEGER NOT NULL DEFAULT 0,
    char_start      INTEGER,
    char_end        INTEGER,
    metadata_json   TEXT,
    vector          BLOB,
    embedding_model TEXT,
    dimensions      INTEGER,
    created_at      TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_chunk_doc ON chunks(document_id);

-- дедуп НЕ удаляет, а помечает (принцип «никогда не терять нюанс»)
CREATE TABLE IF NOT EXISTS chunk_relations (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    chunk_id    INTEGER NOT NULL REFERENCES chunks(id) ON DELETE CASCADE,
    relation    TEXT NOT NULL,
    target_id   INTEGER REFERENCES chunks(id) ON DELETE SET NULL,
    similarity  REAL,
    decided_by  TEXT,
    created_at  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS summaries (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    subject_type  TEXT NOT NULL,
    subject_id    INTEGER NOT NULL,
    text          TEXT NOT NULL,
    facts_json    TEXT,
    entities_json TEXT,
    created_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS embedding_cache (
    sha256      TEXT PRIMARY KEY,
    model       TEXT NOT NULL,
    dim         INTEGER NOT NULL,
    vector      BLOB NOT NULL,
    created_at  TEXT NOT NULL
);
)SQL";

const char *kFts = R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
    text,
    content='chunks',
    content_rowid='id',
    tokenize='unicode61 remove_diacritics 2'
);

CREATE TRIGGER IF NOT EXISTS chunks_ai AFTER INSERT ON chunks BEGIN
    INSERT INTO chunks_fts(rowid, text) VALUES (new.id, new.text);
END;

CREATE TRIGGER IF NOT EXISTS chunks_ad AFTER DELETE ON chunks BEGIN
    INSERT INTO chunks_fts(chunks_fts, rowid, text)
        VALUES ('delete', old.id, old.text);
END;

CREATE TRIGGER IF NOT EXISTS chunks_au AFTER UPDATE OF text ON chunks BEGIN
    INSERT INTO chunks_fts(chunks_fts, rowid, text)
        VALUES ('delete', old.id, old.text);
    INSERT INTO chunks_fts(rowid, text) VALUES (new.id, new.text);
END;
)SQL";

QByteArray packVec(const std::vector<float> &v)
{
    if (v.empty()) return {};

    return QByteArray(
        reinterpret_cast<const char *>(v.data()),
        int(v.size() * sizeof(float))
    );
}

std::vector<float> unpackVec(const QByteArray &b)
{
    std::vector<float> v(size_t(b.size()) / sizeof(float));

    if (!v.empty()) {
        memcpy(v.data(), b.constData(), size_t(b.size()));
    }

    return v;
}

QString toJson(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject fromJson(const QString &s)
{
    if (s.isEmpty()) return {};

    const auto d = QJsonDocument::fromJson(s.toUtf8());
    return d.isObject() ? d.object() : QJsonObject();
}

QString colText(sqlite3_stmt *st, int i)
{
    const unsigned char *p = sqlite3_column_text(st, i);
    return p ? QString::fromUtf8(reinterpret_cast<const char *>(p)) : QString();
}

} // namespace

RagStore::~RagStore()
{
    close();
}

bool RagStore::open(const QString &dbPath, QString *err)
{
    close();

    m_path = dbPath;

    if (sqlite3_open(dbPath.toUtf8().constData(), &m_db) != SQLITE_OK) {
        if (err) *err = QStringLiteral("cannot open %1").arg(dbPath);
        return false;
    }

    // WAL: чтение во время фоновой записи, без блокировок UI.
    exec("PRAGMA journal_mode=WAL;", nullptr);
    exec("PRAGMA synchronous=NORMAL;", nullptr);
    exec("PRAGMA foreign_keys=ON;", nullptr);
    exec("PRAGMA cache_size=-64000;", nullptr);   // 64 MB

    return schemaReady(err);
}

void RagStore::close()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool RagStore::exec(const char *sql, QString *err)
{
    char *msg = nullptr;

    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        if (err) *err = QString::fromUtf8(msg ? msg : "sqlite error");
        sqlite3_free(msg);
        return false;
    }

    sqlite3_free(msg);
    return true;
}

bool RagStore::schemaReady(QString *err)
{
    if (!exec(kSchema, err)) return false;
    if (!exec(kFts, err)) return false;

    // FTS может отстать после падения процесса — синхронизируем.
    exec("INSERT INTO chunks_fts(chunks_fts) VALUES('rebuild');", nullptr);

    return true;
}

qint64 RagStore::findDocByHash(const QString &hash)
{
    sqlite3_stmt *st = nullptr;
    const char *sql = "SELECT id FROM documents WHERE content_hash=?1;";

    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return -1;

    sqlite3_bind_text(st, 1, hash.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    qint64 id = -1;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);

    sqlite3_finalize(st);
    return id;
}

qint64 RagStore::findDocByUri(const QString &uri)
{
    sqlite3_stmt *st = nullptr;
    const char *sql = "SELECT id FROM documents WHERE source_uri=?1 ORDER BY id DESC LIMIT 1;";

    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return -1;

    sqlite3_bind_text(st, 1, uri.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    qint64 id = -1;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);

    sqlite3_finalize(st);
    return id;
}

qint64 RagStore::insertDocument(const DocRecord &doc, QString *err)
{
    const char *sql = R"SQL(
        INSERT INTO documents(
            source_type, source_uri, title, text, content_hash,
            metadata_json, tags, doc_vector, doc_vector_truncated,
            doc_vector_covered, title_vector, embedding_model,
            dimensions, created_at, updated_at
        ) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);
    )SQL";

    sqlite3_stmt *st = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
        return -1;
    }

    const QString typeStr = sourceToString(doc.sourceType);
    const QString metaStr = toJson(doc.metadata);
    const QString tagsStr = doc.tags.join(QLatin1Char(','));
    const QString now = nowIso();
    const QString model = doc.metadata.value(QStringLiteral("embedding_model")).toString();

    sqlite3_bind_text(st, 1, typeStr.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, doc.sourceUri.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, doc.title.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, doc.text.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, doc.contentHash.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, metaStr.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, tagsStr.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    const QByteArray dv = packVec(doc.docVector);
    sqlite3_bind_blob(st, 8, dv.constData(), dv.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 9, doc.docVectorTruncated ? 1 : 0);
    sqlite3_bind_int(st, 10, doc.docVectorCoveredTokens);

    const QByteArray tv = packVec(doc.titleVector);
    sqlite3_bind_blob(st, 11, tv.constData(), tv.size(), SQLITE_TRANSIENT);

    sqlite3_bind_text(st, 12, model.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 13, m_dim);
    sqlite3_bind_text(st, 14, now.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 15, now.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
        return -1;
    }

    return sqlite3_last_insert_rowid(m_db);
}

int RagStore::insertChunks(qint64 docId, const QVector<ChunkRecord> &chunks, QString *err)
{
    const char *sql = R"SQL(
        INSERT INTO chunks(
            document_id, ordinal, text, label, token_est,
            char_start, char_end, metadata_json, vector,
            embedding_model, dimensions, created_at
        ) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);
    )SQL";

    exec("BEGIN;", nullptr);

    sqlite3_stmt *st = nullptr;

    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
        exec("ROLLBACK;", nullptr);
        return -1;
    }

    int inserted = 0;
    const QString now = nowIso();

    for (const ChunkRecord &c : chunks) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);

        const QString metaStr = toJson(c.metadata);
        const QByteArray vec = packVec(c.vector);
        const QString model = c.metadata.value(QStringLiteral("embedding_model")).toString();

        sqlite3_bind_int64(st, 1, docId);
        sqlite3_bind_int(st, 2, c.ordinal);
        sqlite3_bind_text(st, 3, c.text.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, c.label.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, c.tokenEst);
        sqlite3_bind_int(st, 6, c.charStart);
        sqlite3_bind_int(st, 7, c.charEnd);
        sqlite3_bind_text(st, 8, metaStr.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 9, vec.constData(), vec.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 10, model.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 11, m_dim);
        sqlite3_bind_text(st, 12, now.toUtf8().constData(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(st) != SQLITE_DONE) {
            if (err) *err = QString::fromUtf8(sqlite3_errmsg(m_db));
            exec("ROLLBACK;", nullptr);
            sqlite3_finalize(st);
            return -1;
        }

        ++inserted;
    }

    sqlite3_finalize(st);
    exec("COMMIT;", nullptr);

    return inserted;
}

int RagStore::countChunks(qint64 docId)
{
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT count(*) FROM chunks WHERE document_id=?1;",
                       -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, docId);

    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    return n;
}

QVector<qint64> RagStore::chunkIdsOfDoc(qint64 docId, int limit)
{
    QVector<qint64> ids;

    const QString sql = (limit >= 0)
        ? QStringLiteral("SELECT id FROM chunks WHERE document_id=?1 ORDER BY ordinal LIMIT ?2;")
        : QStringLiteral("SELECT id FROM chunks WHERE document_id=?1 ORDER BY ordinal;");

    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, docId);
    if (limit >= 0) sqlite3_bind_int(st, 2, limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(st, 0));
    }

    sqlite3_finalize(st);
    return ids;
}

bool RagStore::getChunksByIds(const QVector<qint64> &ids, QVector<ChunkRecord> *out)
{
    out->clear();
    if (ids.isEmpty()) return true;

    const char *sql = R"SQL(
        SELECT id, document_id, ordinal, text, label, token_est,
               char_start, char_end, metadata_json, created_at
        FROM chunks WHERE id=?1;
    )SQL";

    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr);

    for (qint64 id : ids) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, id);

        if (sqlite3_step(st) != SQLITE_ROW) continue;

        ChunkRecord c;
        c.id = sqlite3_column_int64(st, 0);
        c.docId = sqlite3_column_int64(st, 1);
        c.ordinal = sqlite3_column_int(st, 2);
        c.text = colText(st, 3);
        c.label = colText(st, 4);
        c.tokenEst = sqlite3_column_int(st, 5);
        c.charStart = sqlite3_column_int(st, 6);
        c.charEnd = sqlite3_column_int(st, 7);
        c.metadata = fromJson(colText(st, 8));
        c.createdAt = colText(st, 9);

        out->push_back(c);
    }

    sqlite3_finalize(st);
    return true;
}

bool RagStore::updateChunkVector(qint64 id, const std::vector<float> &vector,
                                 const QString &model, int dim)
{
    const char *sql =
        "UPDATE chunks SET vector=?2, embedding_model=?3, dimensions=?4 WHERE id=?1;";

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;

    const QByteArray vec = packVec(vector);

    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_blob(st, 2, vec.constData(), vec.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, model.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, dim);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE;
}

DeleteResult RagStore::deleteDocument(qint64 id)
{
    DeleteResult r;

    const int chunks = countChunks(id);

    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, "DELETE FROM documents WHERE id=?1;", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        r.error = QString::fromUtf8(sqlite3_errmsg(m_db));
        return r;
    }

    r.ok = true;
    r.documents = 1;
    r.chunks = chunks;   // каскад по FK

    return r;
}

DeleteResult RagStore::deleteDocumentsByUris(const QStringList &uris)
{
    DeleteResult r;

    for (const QString &u : uris) {
        const qint64 id = findDocByUri(u);
        if (id < 0) continue;

        const auto one = deleteDocument(id);
        if (!one.ok) { r.error = one.error; return r; }

        r.documents += one.documents;
        r.chunks += one.chunks;
    }

    r.ok = true;
    return r;
}

bool RagStore::getDocument(qint64 id, DocRecord *out)
{
    const char *sql = R"SQL(
        SELECT id, source_type, source_uri, title, text, content_hash,
               metadata_json, tags, doc_vector, doc_vector_truncated,
               doc_vector_covered, title_vector, created_at, updated_at
        FROM documents WHERE id=?1;
    )SQL";

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int64(st, 1, id);

    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }

    DocRecord d;
    d.id = sqlite3_column_int64(st, 0);
    d.sourceType = sourceFromString(colText(st, 1));
    d.sourceUri = colText(st, 2);
    d.title = colText(st, 3);
    d.text = colText(st, 4);
    d.contentHash = colText(st, 5);
    d.metadata = fromJson(colText(st, 6));
    d.tags = colText(st, 7).split(QLatin1Char(','), Qt::SkipEmptyParts);
    d.docVector = unpackVec(QByteArray(
        reinterpret_cast<const char *>(sqlite3_column_blob(st, 8)),
        sqlite3_column_bytes(st, 8)));
    d.docVectorTruncated = sqlite3_column_int(st, 9) != 0;
    d.docVectorCoveredTokens = sqlite3_column_int(st, 10);
    d.titleVector = unpackVec(QByteArray(
        reinterpret_cast<const char *>(sqlite3_column_blob(st, 11)),
        sqlite3_column_bytes(st, 11)));
    d.createdAt = colText(st, 12);
    d.updatedAt = colText(st, 13);

    sqlite3_finalize(st);

    *out = d;
    return true;
}

QVector<qint64> RagStore::allChunkIds()
{
    QVector<qint64> ids;

    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT id FROM chunks ORDER BY id;", -1, &st, nullptr);

    while (sqlite3_step(st) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(st, 0));
    }

    sqlite3_finalize(st);
    return ids;
}

bool RagStore::loadChunkVectors(QVector<qint64> *ids, std::vector<float> *flat, int *dim)
{
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT id, vector, dimensions FROM chunks WHERE vector IS NOT NULL ORDER BY id;",
        -1, &st, nullptr);

    ids->clear();
    flat->clear();

    int d = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const qint64 id = sqlite3_column_int64(st, 0);

        const QByteArray b(
            reinterpret_cast<const char *>(sqlite3_column_blob(st, 1)),
            sqlite3_column_bytes(st, 1));

        const int cd = sqlite3_column_int(st, 2);

        if (d == 0) d = cd > 0 ? cd : int(b.size() / sizeof(float));

        if (int(b.size() / sizeof(float)) != d) {
            continue;   // вектор другой размерности — пропускаем, не ломаем индекс
        }

        ids->push_back(id);
        const size_t off = flat->size();
        flat->resize(off + size_t(d));
        memcpy(flat->data() + off, b.constData(), size_t(b.size()));
    }

    sqlite3_finalize(st);

    *dim = d;
    if (d > 0) m_dim = d;

    return true;
}

QVector<std::pair<qint64, double>> RagStore::ftsSearch(const QString &q, int limit)
{
    QVector<std::pair<qint64, double>> out;

    // bm25() в SQLite: меньше = лучше. Инвертируем в «больше = лучше».
    const char *sql = R"SQL(
        SELECT rowid, -bm25(chunks_fts) AS score
        FROM chunks_fts
        WHERE chunks_fts MATCH ?1
        ORDER BY score DESC
        LIMIT ?2;
    )SQL";

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return out;

    sqlite3_bind_text(st, 1, q.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back({sqlite3_column_int64(st, 0), sqlite3_column_double(st, 1)});
    }

    sqlite3_finalize(st);
    return out;
}

StatsInfo RagStore::stats()
{
    StatsInfo s;

    sqlite3_stmt *st = nullptr;

    sqlite3_prepare_v2(m_db, "SELECT count(*) FROM documents;", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) s.documents = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(m_db, "SELECT count(*) FROM chunks;", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) s.chunks = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(m_db,
        "SELECT min(created_at), max(created_at) FROM documents;", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) {
        s.oldestDoc = colText(st, 0);
        s.newestDoc = colText(st, 1);
    }
    sqlite3_finalize(st);

    s.dimensions = m_dim;
    s.dbBytes = QFileInfo(m_path).size();

    return s;
}

QVector<DocRecord> RagStore::listDocs(int offset, int limit, int *total)
{
    QVector<DocRecord> out;

    if (total) {
        *total = 0;
        sqlite3_stmt *tc = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT count(*) FROM documents;", -1, &tc, nullptr) == SQLITE_OK) {
            if (sqlite3_step(tc) == SQLITE_ROW) *total = sqlite3_column_int(tc, 0);
        }
        sqlite3_finalize(tc);
    }

    const char *sql = R"SQL(
        SELECT d.id, d.source_type, d.source_uri, d.title, d.tags,
               d.created_at, d.updated_at, d.doc_vector_truncated,
               d.doc_vector_covered,
               (SELECT count(*) FROM chunks c WHERE c.document_id = d.id) AS chunks
        FROM documents d
        ORDER BY d.updated_at DESC, d.id DESC
        LIMIT ?1 OFFSET ?2;
    )SQL";

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return out;

    sqlite3_bind_int(st, 1, limit);
    sqlite3_bind_int(st, 2, offset);

    while (sqlite3_step(st) == SQLITE_ROW) {
        DocRecord d;
        d.id                    = sqlite3_column_int64(st, 0);
        d.sourceType            = sourceFromString(colText(st, 1));
        d.sourceUri             = colText(st, 2);
        d.title                 = colText(st, 3);
        d.tags                  = colText(st, 4).split(QLatin1Char(','), Qt::SkipEmptyParts);
        d.createdAt             = colText(st, 5);
        d.updatedAt             = colText(st, 6);
        d.docVectorTruncated    = sqlite3_column_int(st, 7) != 0;
        d.docVectorCoveredTokens = sqlite3_column_int(st, 8);
        // text не тянем: список лёгкий, полный текст берём в open()
        out.push_back(d);
    }

    sqlite3_finalize(st);
    return out;
}

bool RagStore::updateDocumentTags(qint64 id, const QStringList &tags)
{
    const char *sql = "UPDATE documents SET tags=?2, updated_at=?3 WHERE id=?1;";

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;

    const QString joined = tags.join(QLatin1Char(','));
    const QString now = nowIso();

    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, joined.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, now.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE;
}

} // namespace rag