#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QVector>

#include <vector>

namespace rag {

enum class SourceType {
    Docs,
    Notes,
    Code,
    ChatHistory,
    WebClip,
    Fact,
    Unknown
};

QString sourceToString(SourceType t);
SourceType sourceFromString(const QString &s);

// Доверие источника как множитель финального скора.
// fact/docs — рукотворные и точные; chat_history — шумный; web_clip — наименее достоверный.
double sourceTrust(SourceType t);

struct ChunkDraft {
    QString text;
    QString label;          // heading_path / имя символа
    int tokenEst = 0;
    int charStart = -1;
    int charEnd = -1;
    QJsonObject metadata;
};

struct DocRecord {
    qint64 id = -1;
    SourceType sourceType = SourceType::Unknown;
    QString sourceUri;
    QString title;
    QString text;           // полный текст статьи
    QString contentHash;    // sha256 нормализованного text
    QJsonObject metadata;
    QStringList tags;

    // Вектор по всему содержанию. Честный флаг усечения, если не влез в окно модели.
    std::vector<float> docVector;
    bool docVectorTruncated = false;
    int docVectorCoveredTokens = 0;

    std::vector<float> titleVector;

    QString createdAt;
    QString updatedAt;
};

struct ChunkRecord {
    qint64 id = -1;
    qint64 docId = -1;
    int ordinal = 0;
    QString text;
    QString label;
    int tokenEst = 0;
    int charStart = -1;
    int charEnd = -1;
    QJsonObject metadata;
    std::vector<float> vector;
    QString createdAt;
};

struct SearchQuery {
    QString query;
    int limit = 6;
    int tokenBudget = 2400;     // потолок суммарных токенов выдачи
    int candidatePool = 64;     // сколько тянем из каждого списка до слияния
    int maxSnippetChars = 1400; // обрезка одного сниппета
    QStringList sources;        // фильтр по source_type, пусто = все
    QStringList tags;           // фильтр по тегам, пусто = все
    bool recencyBoost = true;
};

struct Candidate {
    qint64 chunkId = -1;
    qint64 docId = -1;

    int vectorRank = -1;
    int lexicalRank = -1;

    float vectorScore = 0.f;
    float fusedScore = 0.f;     // RRF
    float finalScore = 0.f;     // == fusedScore (RRF — первичный сигнал)
    float trust = 1.f;          // tie-breaker: доверие источника
    float recency = 1.f;        // tie-breaker: свежесть
};

struct Snippet {
    int rank = 0;
    qint64 chunkId = -1;
    qint64 docId = -1;
    QString sourceType;
    QString sourceUri;
    QString title;
    QString label;
    QString text;
    bool truncated = false;
    float score = 0.f;        // RRF × trust × recency
    float vectorScore = 0.f;  // сырой косинус (dot на нормализованных)
    int tokenEst = 0;
    int vectorRank = -1;
    int lexicalRank = -1;
    QString createdAt;
    QJsonObject metadata;
};

struct InsertResult {
    bool ok = false;
    qint64 docId = -1;
    bool unchanged = false;     // тот же hash — ничего не переписывали
    int chunks = 0;
    bool docVectorTruncated = false;
    qint64 embedMs = 0;
    qint64 totalMs = 0;
    QString error;              // не летит в UI, только в логи/CLI
    QStringList warnings;
};

struct DeleteResult {
    bool ok = false;
    int documents = 0;
    int chunks = 0;
    QString error;
};

struct SearchOutcome {
    bool ok = false;
    QVector<Snippet> snippets;

    int indexedChunks = 0;
    int vectorCandidates = 0;
    int lexicalCandidates = 0;
    int droppedByBudget = 0;

    qint64 embedMs = 0;
    qint64 searchMs = 0;
    qint64 totalMs = 0;

    QString error;
    QStringList warnings;
};

struct StatsInfo {
    int documents = 0;
    int chunks = 0;
    long long dbBytes = 0;
    int indexChunks = 0;        // сколько векторов сейчас в памяти
    int dimensions = 0;
    QString embeddingModel;
    QString provider;
    QString pooling;
    QString oldestDoc;
    QString newestDoc;
};

} // namespace rag