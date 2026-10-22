#include "rag/embedding_llamaserver.h"
#include "rag/embedding_provider.h"
#include "rag/rag_engine.h"
#include "rag/rag_util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QThread>

#include <memory>

using namespace rag;

namespace {

QTextStream &out() { static QTextStream s(stdout); return s; }
QTextStream &err() { static QTextStream s(stderr); return s; }

QString defaultDbPath()
{
    return QDir::homePath() + QStringLiteral("/.local/share/watcher/rag.db");
}

void usage()
{
    out() << "rag-cli - автономный RAG-модуль Watcher (llama-server как субпроцесс)\n\n";
    out() << "Использование:\n";
    out() << "  rag-cli [--db PATH] [--llama-server BIN] [--model GGUF] [--port N]\n";
    out() << "          [--max-tokens N] [--threads N] [--pooling cls|mean|last] <command>\n\n";
    out() << "  llama-server берётся из --llama-server / $WATCHER_LLAMA_SERVER / PATH.\n";
    out() << "  Это прекомпилированный бинарник: проект его НЕ компилирует.\n";
    out() << "  Поднимается ребёнком на время батча эмбеддингов и убивается.\n\n";
    out() << "Команды:\n";
    out() << "  insert <file|-> [--title T] [--type docs|notes|code|chat_history|web_clip|fact]\n";
    out() << "         [--tags a,b] [--uri URI]\n";
    out() << "  delete --id N | --uri URI          (модели НЕ доступна)\n";
    out() << "  search <query> [--limit N] [--budget N] [--sources a,b] [--tags a,b]\n";
    out() << "         [--pool N] [--json]         (единственная операция для LLM-агента)\n";
    out() << "  doc <id>\n";
    out() << "  stats\n";
    out() << "  reindex\n";
    out() << "  repl\n\n";
    out() << "Примеры:\n";
    out() << "  rag-cli --model ~/models/bge-m3-q8_0.gguf insert notes/arch.md --type docs --tags watcher\n";
    out() << "  rag-cli --model ~/models/bge-m3-q8_0.gguf search 'как устроен ChatEngine' --limit 5\n\n";
}

std::unique_ptr<EmbeddingProvider> makeProvider(
    const QString &serverBin, const QString &modelPath,
    int maxTok, int threads, int port, Pooling pooling)
{
    QString bin = serverBin;
    if (bin.isEmpty()) bin = qEnvironmentVariable("WATCHER_LLAMA_SERVER");
    if (bin.isEmpty()) bin = QStringLiteral("llama-server");

    const QString gguf = modelPath.isEmpty()
        ? QDir::homePath() + QStringLiteral("/models/bge-m3-q8_0.gguf")
        : modelPath;

    return std::make_unique<LlamaServerEmbeddingProvider>(
        bin, gguf, QFileInfo(gguf).fileName(),
        maxTok > 0 ? maxTok : 8192,
        threads > 0 ? threads : QThread::idealThreadCount(),
        port, pooling);
}

void printSnippet(const Snippet &s)
{
    out() << QString::number(s.rank).rightJustified(2, QLatin1Char(' '))
          << ". [rrf=" << QString::number(s.score, 'f', 4)
          << " cos=" << QString::number(s.vectorScore, 'f', 4) << "] "
          << s.sourceType << " . " << s.title << "\n";

    if (!s.label.isEmpty()) out() << "     " << s.label << "\n";
    if (!s.sourceUri.isEmpty()) out() << "     " << s.sourceUri << "\n";

    const QStringList lines = s.text.split(QLatin1Char('\n'));
    for (const QString &l : lines) out() << "     | " << l << "\n";

    if (s.truncated) out() << "     | ...(обрезано)\n";

    out() << "\n";
}

int cmdInsert(RagEngine &e, const QStringList &args)
{
    if (args.isEmpty()) { err() << "insert: нужен файл или '-'\n"; return 2; }

    QString text;

    if (args.first() == QLatin1String("-")) {
        QTextStream in(stdin);
        text = in.readAll();
    } else {
        QFile f(args.first());
        if (!f.open(QIODevice::ReadOnly)) {
            err() << "insert: не открылся " << args.first() << "\n";
            return 2;
        }
        text = QString::fromUtf8(f.readAll());
    }

    QString title, typeStr, uri = args.first();
    QStringList tags;

    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        if (a == QLatin1String("--title") && i + 1 < args.size()) title = args[++i];
        else if (a == QLatin1String("--type") && i + 1 < args.size()) typeStr = args[++i];
        else if (a == QLatin1String("--tags") && i + 1 < args.size())
            tags = args[++i].split(QLatin1Char(','), Qt::SkipEmptyParts);
        else if (a == QLatin1String("--uri") && i + 1 < args.size()) uri = args[++i];
    }

    const SourceType st = typeStr.isEmpty() ? SourceType::Unknown : sourceFromString(typeStr);
    const auto r = e.insert_data(title, text, st, uri, tags, {});

    if (!r.ok) { err() << "insert failed: " << r.error << "\n"; return 1; }

    out() << "doc " << r.docId
          << (r.unchanged ? " (дубликат, без изменений)" : "")
          << ", chunks " << r.chunks
          << ", embed " << r.embedMs << " ms"
          << ", total " << r.totalMs << " ms\n";

    for (const QString &w : r.warnings) out() << "  warn: " << w << "\n";

    return 0;
}

int cmdDelete(RagEngine &e, const QStringList &args)
{
    DeleteResult r;

    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--id") && i + 1 < args.size())
            r = e.delete_data(args[++i].toLongLong());
        else if (args[i] == QLatin1String("--uri") && i + 1 < args.size())
            r = e.delete_data_by_uri(args[++i]);
    }

    if (!r.ok) { err() << "delete failed: " << r.error << "\n"; return 1; }

    out() << "deleted docs " << r.documents << ", chunks " << r.chunks << "\n";
    return 0;
}

int cmdSearch(RagEngine &e, const QStringList &args)
{
    SearchQuery q;
    QStringList pos;
    bool asJson = false;

    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        if (a == QLatin1String("--limit") && i + 1 < args.size()) q.limit = args[++i].toInt();
        else if (a == QLatin1String("--budget") && i + 1 < args.size()) q.tokenBudget = args[++i].toInt();
        else if (a == QLatin1String("--sources") && i + 1 < args.size())
            q.sources = args[++i].split(QLatin1Char(','), Qt::SkipEmptyParts);
        else if (a == QLatin1String("--tags") && i + 1 < args.size())
            q.tags = args[++i].split(QLatin1Char(','), Qt::SkipEmptyParts);
        else if (a == QLatin1String("--pool") && i + 1 < args.size()) q.candidatePool = args[++i].toInt();
        else if (a == QLatin1String("--json")) asJson = true;
        else pos << a;
    }

    q.query = pos.join(QLatin1Char(' '));

    if (q.query.trimmed().isEmpty()) { err() << "search: пустой запрос\n"; return 2; }

    const auto r = e.rag_search(q);

    if (!r.ok) { err() << "search failed: " << r.error << "\n"; return 1; }

    if (asJson) {
        QJsonArray arr;
        for (const Snippet &s : r.snippets) {
            QJsonObject o;
            o.insert(QStringLiteral("rank"), s.rank);
            o.insert(QStringLiteral("chunk_id"), double(s.chunkId));
            o.insert(QStringLiteral("doc_id"), double(s.docId));
            o.insert(QStringLiteral("source_type"), s.sourceType);
            o.insert(QStringLiteral("source_uri"), s.sourceUri);
            o.insert(QStringLiteral("title"), s.title);
            o.insert(QStringLiteral("label"), s.label);
            o.insert(QStringLiteral("text"), s.text);
            o.insert(QStringLiteral("score"), double(s.score));
            o.insert(QStringLiteral("token_est"), s.tokenEst);
            o.insert(QStringLiteral("truncated"), s.truncated);
            o.insert(QStringLiteral("metadata"), s.metadata);
            arr.push_back(o);
        }

        QJsonObject root;
        root.insert(QStringLiteral("ok"), true);
        root.insert(QStringLiteral("query"), q.query);
        root.insert(QStringLiteral("results"), arr);
        root.insert(QStringLiteral("indexed_chunks"), r.indexedChunks);
        root.insert(QStringLiteral("vector_candidates"), r.vectorCandidates);
        root.insert(QStringLiteral("lexical_candidates"), r.lexicalCandidates);
        root.insert(QStringLiteral("dropped_by_budget"), r.droppedByBudget);
        root.insert(QStringLiteral("embed_ms"), double(r.embedMs));
        root.insert(QStringLiteral("search_ms"), double(r.searchMs));

        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out().flush();
        return 0;
    }

    out() << "\n" << r.snippets.size() << " результатов"
          << "  (индекс: " << r.indexedChunks << " чанков"
          << ", вектор: " << r.vectorCandidates
          << ", bm25: " << r.lexicalCandidates
          << ", бюджет отсёк: " << r.droppedByBudget
          << ")  embed " << r.embedMs << " ms"
          << ", поиск " << r.searchMs << " ms\n\n";

    for (const Snippet &s : r.snippets) printSnippet(s);

    return 0;
}

int cmdDoc(RagEngine &e, const QStringList &args)
{
    if (args.isEmpty()) return 2;

    DocRecord d;
    if (!e.document(args.first().toLongLong(), &d)) {
        err() << "doc: не найден " << args.first() << "\n";
        return 1;
    }

    out() << "\nid:        " << d.id << "\n"
          << "title:     " << d.title << "\n"
          << "type:      " << sourceToString(d.sourceType) << "\n"
          << "uri:       " << d.sourceUri << "\n"
          << "tags:      " << d.tags.join(", ") << "\n"
          << "hash:      " << d.contentHash << "\n"
          << "created:   " << d.createdAt << "\n"
          << "updated:   " << d.updatedAt << "\n"
          << "doc_vec:   " << (d.docVectorTruncated ? "усечён" : "полный")
          << " (" << d.docVectorCoveredTokens << " токенов)\n"
          << "\n--- text ---\n" << d.text << "\n";

    return 0;
}

int cmdStats(RagEngine &e)
{
    const auto s = e.stats();

    out() << "\ndocuments:        " << s.documents << "\n"
          << "chunks:           " << s.chunks << "\n"
          << "index in memory:  " << s.indexChunks << "\n"
          << "dimensions:       " << s.dimensions << "\n"
          << "db bytes:         " << s.dbBytes << "\n"
          << "embedding model:  " << s.embeddingModel << "\n"
          << "provider:         " << s.provider << "\n"
          << "pooling:          " << s.pooling << "\n"
          << "oldest doc:       " << s.oldestDoc << "\n"
          << "newest doc:       " << s.newestDoc << "\n\n";

    return 0;
}

int cmdReindex(RagEngine &e)
{
    QString eMsg;
    if (!e.reindex(&eMsg)) {
        err() << "reindex failed: " << eMsg << "\n";
        return 1;
    }
    out() << "reindex ok\n";
    return 0;
}

int runRepl(RagEngine &e)
{
    QTextStream in(stdin);
    out() << "rag repl. команды: search <q> | stats | quit\n\n";

    while (!in.atEnd()) {
        out() << "rag> ";
        out().flush();

        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        if (line == QLatin1String("quit") || line == QLatin1String("exit")) break;
        if (line == QLatin1String("stats")) { cmdStats(e); continue; }
        if (line.startsWith(QLatin1String("search "))) { cmdSearch(e, {line.mid(7)}); continue; }

        out() << "неизвестная команда\n";
    }

    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QStringList all;
    for (int i = 0; i < argc; ++i) all << QString::fromUtf8(argv[i]);

    QString db = defaultDbPath();
    QString serverBin, modelPath;
    int maxTok = 0, threads = 0, port = 0;
    Pooling pooling = Pooling::Cls;

    QStringList rest;

    for (int i = 1; i < all.size(); ++i) {
        const QString &a = all[i];
        if (a == QLatin1String("--db") && i + 1 < all.size()) db = all[++i];
        else if (a == QLatin1String("--llama-server") && i + 1 < all.size()) serverBin = all[++i];
        else if (a == QLatin1String("--model") && i + 1 < all.size()) modelPath = all[++i];
        else if (a == QLatin1String("--port") && i + 1 < all.size()) port = all[++i].toInt();
        else if (a == QLatin1String("--max-tokens") && i + 1 < all.size()) maxTok = all[++i].toInt();
        else if (a == QLatin1String("--threads") && i + 1 < all.size()) threads = all[++i].toInt();
        else if (a == QLatin1String("--pooling") && i + 1 < all.size()) pooling = poolingFromString(all[++i]);
        else rest << a;
    }

    if (rest.isEmpty()) { usage(); return 1; }

    QDir().mkpath(QFileInfo(db).absolutePath());

    auto provider = makeProvider(serverBin, modelPath, maxTok, threads, port, pooling);

    RagEngine engine;
    QString eMsg;

    if (!engine.init(db, provider.release(), &eMsg)) {
        err() << "init failed: " << eMsg << "\n";
        return 1;
    }

    const QString cmd = rest.takeFirst();

    if (cmd == QLatin1String("insert"))  return cmdInsert(engine, rest);
    if (cmd == QLatin1String("delete"))  return cmdDelete(engine, rest);
    if (cmd == QLatin1String("search"))  return cmdSearch(engine, rest);
    if (cmd == QLatin1String("doc"))     return cmdDoc(engine, rest);
    if (cmd == QLatin1String("stats"))   return cmdStats(engine);
    if (cmd == QLatin1String("reindex")) return cmdReindex(engine);
    if (cmd == QLatin1String("repl"))    return runRepl(engine);

    usage();
    return 1;
}
