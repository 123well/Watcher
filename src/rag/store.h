#pragma once

#include "rag_types.h"

#include <QString>
#include <QVector>
#include <QStringList>

#include <utility>

struct sqlite3;

namespace rag {

class RagStore {
public:
    ~RagStore();

    bool open(const QString &dbPath, QString *err);
    void close();

    bool schemaReady(QString *err);

    // documents
    qint64 findDocByHash(const QString &hash);
    qint64 findDocByUri(const QString &uri);
    qint64 insertDocument(const DocRecord &doc, QString *err);
    bool getDocument(qint64 id, DocRecord *out);

    // chunks
    int insertChunks(qint64 docId, const QVector<ChunkRecord> &chunks, QString *err);
    int countChunks(qint64 docId);
    QVector<qint64> chunkIdsOfDoc(qint64 docId, int limit = -1);
    bool getChunksByIds(const QVector<qint64> &ids, QVector<ChunkRecord> *out);
    bool updateChunkVector(qint64 id, const std::vector<float> &vector,
                           const QString &model, int dim);

    // delete
    DeleteResult deleteDocument(qint64 id);
    DeleteResult deleteDocumentsByUris(const QStringList &uris);

    // индекс
    QVector<qint64> allChunkIds();
    QVector<DocRecord> listDocs(int offset, int limit, int *total);
    bool updateDocumentTags(qint64 id, const QStringList &tags);
    bool loadChunkVectors(QVector<qint64> *ids, std::vector<float> *flat, int *dim);

    // FTS
    QVector<std::pair<qint64, double>> ftsSearch(const QString &query, int limit);

    StatsInfo stats();

    int dimensions() const { return m_dim; }
    void setDimensions(int d) { m_dim = d; }

    const QString &path() const { return m_path; }

private:
    bool exec(const char *sql, QString *err);

    sqlite3 *m_db = nullptr;
    QString m_path;
    int m_dim = 0;
};

} // namespace rag