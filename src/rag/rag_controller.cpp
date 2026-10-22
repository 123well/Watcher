#include "rag_controller.h"

#include "embedding_llamaserver.h"
#include "rag_engine.h"
#include "rag_types.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QStringList>
#include <QHash>

namespace rag {

// ─────────────────────────────────────────────────────────────────────────────
// Воркер: единственный, кто трогает RagEngine. Живёт в m_thread.
// ─────────────────────────────────────────────────────────────────────────────
class RagWorker : public QObject {
    Q_OBJECT

public:
    explicit RagWorker(QObject *parent = nullptr) : QObject(parent) {}

    ~RagWorker() override
    {
        // engine/provider уничтожаются здесь же, в своём потоке, если shutdown
        // не был вызван (защита от "QProcess destroyed from another thread").
        m_engine.reset();
    }

public slots:
    void configure(const QString &dbPath, const QString &serverBin,
                   const QString &modelPath, int maxTokens, int threads)
    {
        QString err;

        if (!QFileInfo::exists(modelPath)) {
            emit readyChanged(false, QStringLiteral("GGUF не найден: %1").arg(modelPath));
            return;
        }

        const QString resolvedServer = resolveServer(serverBin);
        if (resolvedServer.isEmpty()) {
            emit readyChanged(false,
                QStringLiteral("llama-server не найден. Поставь WATCHER_LLAMA_SERVER "
                               "или положи llama-server в PATH (пробовал: %1)").arg(serverBin));
            return;
        }

        auto provider = std::make_unique<LlamaServerEmbeddingProvider>(
            resolvedServer, modelPath, QFileInfo(modelPath).fileName(),
            maxTokens > 0 ? maxTokens : 8192,
            threads > 0 ? threads : QThread::idealThreadCount(),
            0, Pooling::Cls);

        m_engine = std::make_unique<RagEngine>();

        if (!m_engine->init(dbPath, provider.release(), &err)) {
            m_engine.reset();
            emit readyChanged(false, QStringLiteral("init rag: %1").arg(err));
            return;
        }

        m_dims = m_engine->provider()->dimensions();
        emit readyChanged(true, QString());
    }

    void shutdown() { m_engine.reset(); }

    void browse(int page, int pageSize)
    {
        if (!m_engine) { emit failed(QStringLiteral("browse"), QStringLiteral("rag не инициализирован")); return; }

        int total = 0;
        const int off = qMax(0, page) * qMax(1, pageSize);
        const auto docs = m_engine->list_docs(off, qMax(1, pageSize), &total);

        QVariantList items;
        for (const DocRecord &d : docs) {
            QVariantMap m;
            m.insert(QStringLiteral("docId"),     double(d.id));
            m.insert(QStringLiteral("title"),     d.title.isEmpty() ? QStringLiteral("(без названия)") : d.title);
            m.insert(QStringLiteral("type"),      sourceToString(d.sourceType));
            m.insert(QStringLiteral("uri"),       d.sourceUri);
            m.insert(QStringLiteral("tags"),      QVariantList(QStringList(d.tags).begin(), QStringList(d.tags).end()));
            m.insert(QStringLiteral("score"),     -1.0);
            m.insert(QStringLiteral("snippet"),   QString());
            m.insert(QStringLiteral("chunks"),    m_engine->chunk_count(d.id));
            m.insert(QStringLiteral("updatedAt"), d.updatedAt);
            items.push_back(m);
        }

        const int pc = qMax(1, (total + pageSize - 1) / qMax(1, pageSize));
        emit itemsReady(QStringLiteral("browse"), items, page, pc, total, QString());
    }

    void search(const QString &q, int pageSize)
    {
        if (!m_engine) { emit failed(QStringLiteral("search"), QStringLiteral("rag не инициализирован")); return; }
        if (q.trimmed().isEmpty()) { emit failed(QStringLiteral("search"), QStringLiteral("пустой запрос")); return; }

        SearchQuery sq;
        sq.query = q;
        sq.limit = qMax(pageSize * 3, 24);   // чанков тянем шире, чем потом документов
        sq.tokenBudget = 3000;
        sq.candidatePool = 64;
        sq.maxSnippetChars = 260;            // для строки списка, не для модели

        const auto r = m_engine->rag_search(sq);

        if (!r.ok) {
            emit failed(QStringLiteral("search"), r.error.isEmpty() ? QStringLiteral("поиск не удался") : r.error);
            return;
        }

        // Агрегация чанков к документам: лучший скор и его сниппет.
        // Порядок сниппетов уже отсортирован по finalScore, поэтому первая
        // встреча документа и есть его лучшее место.
        QVariantList items;
        QHash<qint64, int> seen;

        for (const Snippet &s : r.snippets) {
            int idx = -1;
            auto it = seen.find(s.docId);
            if (it != seen.end()) idx = it.value();

            if (idx < 0) {
                QVariantMap m;
                m.insert(QStringLiteral("docId"),   double(s.docId));
                m.insert(QStringLiteral("title"),   s.title.isEmpty() ? QStringLiteral("(без названия)") : s.title);
                m.insert(QStringLiteral("type"),    s.sourceType);
                m.insert(QStringLiteral("uri"),     s.sourceUri);
                m.insert(QStringLiteral("tags"),    QVariantList());
                m.insert(QStringLiteral("score"),   double(s.score));
                m.insert(QStringLiteral("snippet"), s.text);
                m.insert(QStringLiteral("chunks"),  0);
                m.insert(QStringLiteral("updatedAt"), s.createdAt);
                items.push_back(m);
                idx = items.size() - 1;
                seen.insert(s.docId, idx);
            } else {
                QVariantMap m = items[idx].toMap();
                if (s.score > m.value(QStringLiteral("score")).toDouble()) {
                    m.insert(QStringLiteral("score"), double(s.score));
                    m.insert(QStringLiteral("snippet"), s.text);
                    items[idx] = m;
                }
            }
        }

        if (items.size() > pageSize) items = items.mid(0, pageSize);

        emit itemsReady(QStringLiteral("search"), items, 0, 1, items.size(), q);
    }

    void open(qint64 id)
    {
        if (!m_engine) { emit failed(QStringLiteral("open"), QStringLiteral("rag не инициализирован")); return; }

        DocRecord d;
        if (!m_engine->document(id, &d)) {
            emit failed(QStringLiteral("open"), QStringLiteral("документ %1 не найден").arg(id));
            return;
        }

        emit docReady(docToMap(d, m_engine->chunk_count(id)));
    }

    void createDoc(const QString &title, const QString &type, const QString &tags,
                   const QString &text)
    {
        if (!m_engine) { emit failed(QStringLiteral("create"), QStringLiteral("rag не инициализирован")); return; }

        const QStringList tagList = splitTags(tags);
        QJsonObject meta;
        meta.insert(QStringLiteral("origin"), QStringLiteral("manual"));

        const auto r = m_engine->insert_data(title, text, sourceFromString(type),
                                             QString(), tagList, meta);
        if (!r.ok) {
            emit failed(QStringLiteral("create"), r.error);
            return;
        }

        emit saved(double(r.docId), true);
    }

    void saveDoc(qint64 id, const QString &title, const QString &tags, const QString &text)
    {
        if (!m_engine) { emit failed(QStringLiteral("save"), QStringLiteral("rag не инициализирован")); return; }

        DocRecord cur;
        if (!m_engine->document(id, &cur)) {
            emit failed(QStringLiteral("save"), QStringLiteral("документ %1 не найден").arg(id));
            return;
        }

        const bool textChanged  = cur.text != text;
        const bool titleChanged = cur.title != title;
        const QStringList tagList = splitTags(tags);

        // Дешёвый путь: только теги. Title входит в представление документа,
        // поэтому его правка меняет doc/title-вектор -> нужен переэмбеддинг.
        if (!textChanged && !titleChanged) {
            if (cur.tags != tagList) {
                if (!m_engine->update_tags(id, tagList)) {
                    emit failed(QStringLiteral("save"), QStringLiteral("не обновил теги"));
                    return;
                }
            }
            emit saved(double(id), false);
            return;
        }

        // Полный путь: delete + insert. Идемпотентно, пересчитывает всё.
        // Минус v1: теряются chunk_relations (их ещё нет) и сам id.
        m_engine->delete_data(id);

        const auto r = m_engine->insert_data(title, text, cur.sourceType,
                                             cur.sourceUri, tagList, cur.metadata);
        if (!r.ok) {
            emit failed(QStringLiteral("save"),
                        QStringLiteral("документ удалён, но пересоздание упало: %1").arg(r.error));
            return;
        }

        emit saved(double(r.docId), true);
    }

    void deleteDoc(qint64 id)
    {
        if (!m_engine) { emit failed(QStringLiteral("delete"), QStringLiteral("rag не инициализирован")); return; }

        const auto r = m_engine->delete_data(id);
        if (!r.ok) { emit failed(QStringLiteral("delete"), r.error); return; }

        emit deleted(double(id));
    }

    void stats()
    {
        if (!m_engine) { emit failed(QStringLiteral("stats"), QStringLiteral("rag не инициализирован")); return; }

        const StatsInfo s = m_engine->stats();

        QVariantMap m;
        m.insert(QStringLiteral("documents"), s.documents);
        m.insert(QStringLiteral("chunks"),    s.chunks);
        m.insert(QStringLiteral("indexChunks"), s.indexChunks);
        m.insert(QStringLiteral("dimensions"), s.dimensions > 0 ? s.dimensions : m_dims);
        m.insert(QStringLiteral("dbBytes"),   double(s.dbBytes));
        m.insert(QStringLiteral("model"),     s.embeddingModel);
        m.insert(QStringLiteral("provider"),  s.provider);
        m.insert(QStringLiteral("pooling"),   s.pooling);

        emit statsReady(m);
    }

signals:
    void readyChanged(bool ok, const QString &err);
    void itemsReady(const QString &mode, const QVariantList &items, int page,
                    int pageCount, int total, const QString &query);
    void docReady(const QVariantMap &doc);
    void saved(double newId, bool reembedded);
    void deleted(double id);
    void statsReady(const QVariantMap &s);
    void failed(const QString &what, const QString &err);

private:
    static QString resolveServer(const QString &bin)
    {
        if (bin.isEmpty()) return QString();
        if (bin.contains(QLatin1Char('/'))) {
            return QFileInfo::exists(bin) ? bin : QString();
        }
        // голое имя -> ищем в PATH (QFileInfo::exists("llama-server") не сработал бы)
        return QStandardPaths::findExecutable(bin);
    }

    static QStringList splitTags(const QString &raw)
    {
        return raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }

    static QVariantMap docToMap(const DocRecord &d, int chunks)
    {
        QVariantMap m;
        m.insert(QStringLiteral("docId"),    double(d.id));
        m.insert(QStringLiteral("title"),    d.title);
        m.insert(QStringLiteral("type"),     sourceToString(d.sourceType));
        m.insert(QStringLiteral("uri"),      d.sourceUri);
        m.insert(QStringLiteral("tags"),     QVariantList(QStringList(d.tags).begin(), QStringList(d.tags).end()));
        m.insert(QStringLiteral("text"),     d.text);
        m.insert(QStringLiteral("createdAt"), d.createdAt);
        m.insert(QStringLiteral("updatedAt"), d.updatedAt);
        m.insert(QStringLiteral("truncated"), d.docVectorTruncated);
        m.insert(QStringLiteral("covered"),   d.docVectorCoveredTokens);
        m.insert(QStringLiteral("chunks"),    chunks);
        return m;
    }

    std::unique_ptr<RagEngine> m_engine;
    int m_dims = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Контроллер (GUI‑поток)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

QString envOr(const QString &key, const QString &fallback)
{
    const QString v = QProcessEnvironment::systemEnvironment().value(key);
    return v.isEmpty() ? fallback : v;
}

} // namespace

RagController::RagController(QObject *parent) : QObject(parent)
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("rag-worker"));

    // Воркер без родителя-в-потоке-GUI: moveToThread переносит его сам.
    m_worker = std::make_unique<RagWorker>();
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, this, [this] {
        const QString db = envOr(QStringLiteral("WATCHER_RAG_DB"),
                                 QDir::homePath() + QStringLiteral("/.local/share/watcher/rag.db"));
        const QString srv = envOr(QStringLiteral("WATCHER_LLAMA_SERVER"),
                                  QStringLiteral("llama-server"));
        const QString mdl = envOr(QStringLiteral("WATCHER_RAG_MODEL"),
                                  QDir::homePath() + QStringLiteral("/models/bge-m3-q8_0.gguf"));

        QMetaObject::invokeMethod(m_worker.get(), "configure", Qt::QueuedConnection,
                                  Q_ARG(QString, db),
                                  Q_ARG(QString, srv),
                                  Q_ARG(QString, mdl),
                                  Q_ARG(int, 8192),
                                  Q_ARG(int, 0));
    });

    connect(m_worker.get(), &RagWorker::readyChanged, this, &RagController::onReady);
    connect(m_worker.get(), &RagWorker::itemsReady,   this, &RagController::onItems);
    connect(m_worker.get(), &RagWorker::docReady,     this, &RagController::onDoc);
    connect(m_worker.get(), &RagWorker::saved,        this, &RagController::onSaved);
    connect(m_worker.get(), &RagWorker::deleted,      this, &RagController::onDeleted);
    connect(m_worker.get(), &RagWorker::statsReady,   this, &RagController::onStats);
    connect(m_worker.get(), &RagWorker::failed,       this, &RagController::onFailed);

    m_thread->start();
}

RagController::~RagController()
{
    if (m_thread->isRunning()) {
        // Гарантируем, что engine/provider умирают в своём потоке ДО остановки.
        QMetaObject::invokeMethod(m_worker.get(), "shutdown", Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait(5000);
    }
}

// --- счётчики занятости ---
void RagController::submitSlow() { ++m_pending; ++m_slow; emit busyChanged(); emit workingChanged(); }
void RagController::finishSlow()
{
    if (m_slow > 0) --m_slow;
    if (m_pending > 0) --m_pending;
    emit busyChanged();
    emit workingChanged();
}

void RagController::setError(const QString &e)
{
    if (m_error == e) return;
    m_error = e;
    emit errorChanged();
}

// --- слоты ответов ---
void RagController::onReady(bool ok, const QString &err)
{
    m_ready = ok;
    emit readyChanged();

    if (!ok) {
        setError(err);
        return;
    }
    setError(QString());
    browseNow();
    reloadStats();
}

void RagController::onItems(const QString &mode, const QVariantList &items, int page,
                            int pageCount, int total, const QString &query)
{
    m_mode = mode;
    m_items = items;
    m_page = page;
    m_pageCount = pageCount;
    m_totalDocs = total;
    m_query = query;
    if (mode == QLatin1String("browse")) m_page = page;
    emit itemsChanged();
    finishSlow();
}

void RagController::onDoc(const QVariantMap &doc)
{
    m_selected = doc;
    m_editorMode = QStringLiteral("edit");
    emit selectionChanged();
    finishSlow();
}

void RagController::onSaved(double newId, bool reembedded)
{
    finishSlow();
    setError(QString());
    reloadStats();
    browseNow();

    // При переэмбеддинге id сменился (delete+insert) -> переоткрываем на новый.
    if (m_editorMode != QLatin1String("none")) {
        QMetaObject::invokeMethod(this, [this, newId] { openDoc(newId); });
    }
    (void)reembedded;
}

void RagController::onDeleted(double id)
{
    finishSlow();
    setError(QString());

    if (!m_selected.isEmpty() && m_selected.value(QStringLiteral("docId")).toDouble() == id) {
        m_selected.clear();
        m_editorMode = QStringLiteral("none");
        emit selectionChanged();
    }

    reloadStats();
    browseNow();
}

void RagController::onStats(const QVariantMap &s)
{
    m_stats = s;
    emit statsChanged();
    finishSlow();
}

void RagController::onFailed(const QString &what, const QString &err)
{
    finishSlow();
    setError(QStringLiteral("%1: %2").arg(what, err));
}

// --- действия ---
void RagController::browseNow()
{
    if (!m_ready) return;
    ++m_pending;
    emit busyChanged();
    QMetaObject::invokeMethod(m_worker.get(), "browse", Qt::QueuedConnection,
                              Q_ARG(int, m_page), Q_ARG(int, m_pageSize));
}

void RagController::refresh()
{
    if (m_mode == QLatin1String("search") && !m_query.isEmpty()) search(m_query);
    else browseNow();
}

void RagController::search(const QString &q)
{
    if (!m_ready) { setError(QStringLiteral("rag не готов")); return; }
    const QString t = q.trimmed();
    if (t.isEmpty()) { clearSearch(); return; }

    submitSlow();
    setError(QString());
    QMetaObject::invokeMethod(m_worker.get(), "search", Qt::QueuedConnection,
                              Q_ARG(QString, t), Q_ARG(int, m_pageSize));
}

void RagController::clearSearch()
{
    m_mode = QStringLiteral("browse");
    m_query.clear();
    m_page = 0;
    browseNow();
}

void RagController::setPageSize(int n)
{
    const int v = qBound(5, n, 50);
    if (v == m_pageSize) return;
    m_pageSize = v;
    m_page = 0;
    emit itemsChanged();
    browseNow();
}

void RagController::goPage(int p)
{
    const int v = qBound(0, p, qMax(0, m_pageCount - 1));
    if (v == m_page) return;
    m_page = v;
    browseNow();
}

void RagController::nextPage() { goPage(m_page + 1); }
void RagController::prevPage() { goPage(m_page - 1); }

void RagController::openDoc(double id)
{
    if (!m_ready) return;
    ++m_pending;
    emit busyChanged();
    QMetaObject::invokeMethod(m_worker.get(), "open", Qt::QueuedConnection,
                              Q_ARG(qint64, qint64(id)));
}

void RagController::newDoc()
{
    m_selected.clear();
    m_editorMode = QStringLiteral("create");
    emit selectionChanged();
}

void RagController::closeDoc()
{
    m_selected.clear();
    m_editorMode = QStringLiteral("none");
    emit selectionChanged();
}

void RagController::saveDoc(const QString &title, const QString &type,
                            const QString &tags, const QString &text)
{
    if (!m_ready) { setError(QStringLiteral("rag не готов")); return; }
    if (text.trimmed().isEmpty()) { setError(QStringLiteral("пустой текст")); return; }

    submitSlow();
    setError(QString());

    if (m_editorMode == QLatin1String("create")) {
        QMetaObject::invokeMethod(m_worker.get(), "createDoc", Qt::QueuedConnection,
                                  Q_ARG(QString, title), Q_ARG(QString, type),
                                  Q_ARG(QString, tags), Q_ARG(QString, text));
        return;
    }

    const qint64 id = qint64(m_selected.value(QStringLiteral("docId")).toDouble());
    if (id <= 0) { setError(QStringLiteral("нет выбранного документа")); finishSlow(); return; }

    QMetaObject::invokeMethod(m_worker.get(), "saveDoc", Qt::QueuedConnection,
                              Q_ARG(qint64, id), Q_ARG(QString, title),
                              Q_ARG(QString, tags), Q_ARG(QString, text));
}

void RagController::deleteDoc(double id)
{
    if (!m_ready) return;
    submitSlow();
    QMetaObject::invokeMethod(m_worker.get(), "deleteDoc", Qt::QueuedConnection,
                              Q_ARG(qint64, qint64(id)));
}

void RagController::reloadStats()
{
    if (!m_ready) return;
    ++m_pending;
    emit busyChanged();
    QMetaObject::invokeMethod(m_worker.get(), "stats", Qt::QueuedConnection);
}

} // namespace rag

#include "rag_controller.moc"
