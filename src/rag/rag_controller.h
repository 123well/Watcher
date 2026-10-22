#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>

#include <memory>

namespace rag {

class RagWorker;

// GUI‑мост между QML и RagEngine.
//
// RagEngine живёт В РАБОЧЕМ ПОТОКЕ и никогда не трогается отсюда напрямую:
// insert/search поднимают llama-server и блокируют 1.5-3 с, а QProcess внутри
// провайдера имеет аффинность со своим потоком. Контроллер держит только
// состояние (свойства для биндингов QML) и очередь задач.
//
// Каждый джоб в воркере эмитит РОВНО ОДИН терминальный сигнал (result или
// failed) — на этом построен счётчик занятости.
class RagController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool ready       READ ready       NOTIFY readyChanged)
    Q_PROPERTY(bool busy        READ busy        NOTIFY busyChanged)
    Q_PROPERTY(bool working     READ working     NOTIFY workingChanged)
    Q_PROPERTY(QString error    READ error       NOTIFY errorChanged)
    Q_PROPERTY(QString mode     READ mode        NOTIFY itemsChanged)
    Q_PROPERTY(QString query    READ query       NOTIFY itemsChanged)
    Q_PROPERTY(int page         READ page        NOTIFY itemsChanged)
    Q_PROPERTY(int pageSize     READ pageSize    WRITE setPageSize NOTIFY itemsChanged)
    Q_PROPERTY(int pageCount    READ pageCount   NOTIFY itemsChanged)
    Q_PROPERTY(int totalDocs    READ totalDocs   NOTIFY itemsChanged)
    Q_PROPERTY(QVariantList items READ items     NOTIFY itemsChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString editorMode READ editorMode NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap stats  READ stats     NOTIFY statsChanged)

public:
    explicit RagController(QObject *parent = nullptr);
    ~RagController() override;

    bool ready() const { return m_ready; }
    bool busy() const { return m_pending > 0; }
    bool working() const { return m_slow > 0; }
    QString error() const { return m_error; }
    QString mode() const { return m_mode; }
    QString query() const { return m_query; }
    int page() const { return m_page; }
    int pageSize() const { return m_pageSize; }
    int pageCount() const { return m_pageCount; }
    int totalDocs() const { return m_totalDocs; }
    const QVariantList &items() const { return m_items; }
    bool hasSelection() const { return m_editorMode != QLatin1String("none"); }
    QString editorMode() const { return m_editorMode; }
    const QVariantMap &selected() const { return m_selected; }
    const QVariantMap &stats() const { return m_stats; }

    // --- QML API ---
    Q_INVOKABLE void refresh();                       // перечитать текущую страницу
    Q_INVOKABLE void search(const QString &q);        // mode=search
    Q_INVOKABLE void clearSearch();                   // mode=browse
    Q_INVOKABLE void setPageSize(int n);
    Q_INVOKABLE void goPage(int p);
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void prevPage();
    Q_INVOKABLE void openDoc(double id);
    Q_INVOKABLE void newDoc();
    Q_INVOKABLE void closeDoc();
    Q_INVOKABLE void saveDoc(const QString &title, const QString &type,
                             const QString &tags, const QString &text);
    Q_INVOKABLE void deleteDoc(double id);
    Q_INVOKABLE void reloadStats();

signals:
    void readyChanged();
    void busyChanged();
    void workingChanged();
    void errorChanged();
    void itemsChanged();
    void selectionChanged();
    void statsChanged();

private slots:
    void onReady(bool ok, const QString &err);
    void onItems(const QString &mode, const QVariantList &items, int page,
                 int pageCount, int total, const QString &query);
    void onDoc(const QVariantMap &doc);
    void onSaved(double newId, bool reembedded);
    void onDeleted(double id);
    void onStats(const QVariantMap &s);
    void onFailed(const QString &what, const QString &err);

private:
    void submitSlow();
    void finishSlow();
    void setError(const QString &e);
    void browseNow();

    QThread *m_thread = nullptr;
    std::unique_ptr<RagWorker> m_worker;

    bool m_ready = false;
    int m_pending = 0;      // любые джобы
    int m_slow = 0;         // только те, что ждут llama-server
    int m_pageSize = 10;
    int m_page = 0;
    int m_pageCount = 1;
    int m_totalDocs = 0;
    QString m_mode = QStringLiteral("browse");
    QString m_query;
    QString m_error;
    QString m_editorMode = QStringLiteral("none");
    QVariantList m_items;
    QVariantMap m_selected;
    QVariantMap m_stats;
};

} // namespace rag
