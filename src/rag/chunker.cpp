#include "chunker.h"
#include "rag_util.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

namespace rag {

namespace {

const QRegularExpression &mdHeading()
{
    static const QRegularExpression re(QStringLiteral("^(#{1,6})\\s+(.*)$"));
    return re;
}

const QRegularExpression &codeUnit()
{
    // Эвристика по границам объявлений для C/C++/Qt, Python, JS/TS, Rust, Go.
    static const QRegularExpression re(QStringLiteral(
        R"(^[\s]*(?:template\s*<[^>]*>\s*)?"
        r"(?:class|struct|enum|namespace|def|fn|func|function|async\s+function|"
        r"export\s+(?:default\s+)?(?:async\s+)?function|impl|trait|type)\b)"
    ), QRegularExpression::MultilineOption);
    return re;
}

bool looksLikeCode(const QString &uri, const QString &text)
{
    static const QStringList exts = {
        "cpp", "h", "hpp", "cc", "cxx",
        "c", "py", "js", "ts", "tsx", "jsx",
        "rs", "go", "java", "kt", "sh", "bash", "sql"
    };

    const QString suf = QFileInfo(uri).suffix().toLower();

    if (exts.contains(suf)) {
        return true;
    }

    int hits = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));

    for (int i = 0; i < qMin(80, lines.size()); ++i) {
        const QString &l = lines[i];

        if (l.startsWith(QLatin1String("#include")) ||
            l.startsWith(QLatin1String("import ")) ||
            l.startsWith(QLatin1String("from ")) ||
            l.contains(QLatin1String("void ")) ||
            l.contains(QLatin1String("def "))) {
            ++hits;
        }
    }

    return hits >= 2;
}

SourceType detectType(SourceType hint, const QString &uri, const QString &text)
{
    if (hint != SourceType::Unknown) {
        return hint;
    }

    const QString suf = QFileInfo(uri).suffix().toLower();

    if (suf == QLatin1String("md") || suf == QLatin1String("markdown") ||
        suf == QLatin1String("rst") || suf == QLatin1String("txt")) {
        return SourceType::Docs;
    }

    if (looksLikeCode(uri, text)) {
        return SourceType::Code;
    }

    return SourceType::Notes;
}

QString languageOf(const QString &uri)
{
    return QFileInfo(uri).suffix().toLower();
}

// Склеивает маленькие куски в окна ~target с overlap по целым единицам.
// Никогда не режет внутри единицы — это и есть смысл структурного чанкинга.
// Метаданные окна = метаданные первой попавшей в него единицы.
QVector<ChunkDraft> packUnits(
    const QVector<QString> &units,
    const QVector<QString> &labels,
    const QVector<int> &starts,
    const QVector<int> &ends,
    const QVector<QJsonObject> &metas,
    int targetTokens,
    int overlapTokens
)
{
    QVector<ChunkDraft> out;

    QString buf;
    QString label;
    int bufStart = -1;
    int bufEnd = -1;
    int bufTokens = 0;
    int bufMetaIdx = -1;

    auto flush = [&]() {
        if (buf.trimmed().isEmpty()) {
            return;
        }

        ChunkDraft d;
        d.text = buf.trimmed();
        d.label = label;
        d.tokenEst = estimateTokens(d.text);
        d.charStart = bufStart;
        d.charEnd = bufEnd;
        d.metadata = (bufMetaIdx >= 0 && bufMetaIdx < metas.size())
            ? metas[bufMetaIdx]
            : QJsonObject();

        out.push_back(d);
    };

    for (int i = 0; i < units.size(); ++i) {
        const QString &u = units[i];
        const int uTokens = estimateTokens(u);

        // Один юнит больше цели — кладём его отдельно, не режем.
        if (uTokens > targetTokens) {
            flush();

            ChunkDraft d;
            d.text = u.trimmed();
            d.label = labels[i];
            d.tokenEst = uTokens;
            d.charStart = starts[i];
            d.charEnd = ends[i];
            d.metadata = (i < metas.size()) ? metas[i] : QJsonObject();

            out.push_back(d);

            buf.clear();
            label.clear();
            bufStart = -1;
            bufEnd = -1;
            bufTokens = 0;
            bufMetaIdx = -1;

            continue;
        }

        if (bufTokens + uTokens > targetTokens && !buf.isEmpty()) {
            flush();

            // overlap: оставляем хвост предыдущего окна
            const QString tail = buf.right(overlapTokens * 4);
            buf = tail + QLatin1Char('\n') + u;
            bufTokens = estimateTokens(buf);
            bufEnd = ends[i];
            label = labels[i];
            bufMetaIdx = i;
        } else {
            if (buf.isEmpty()) {
                bufStart = starts[i];
                label = labels[i];
                bufMetaIdx = i;
            }

            buf += u;
            buf += QLatin1Char('\n');
            bufTokens += uTokens;
            bufEnd = ends[i];
        }
    }

    flush();

    return out;
}

} // namespace

ChunkPlan chunkDocument(
    SourceType hint,
    const QString &sourceUri,
    const QString &text,
    int targetTokenSize,
    int overlapTokens
)
{
    ChunkPlan plan;

    plan.detected = detectType(hint, sourceUri, text);

    const QStringList lines = text.split(QLatin1Char('\n'));

    QVector<int> lineOffsets(lines.size());
    int acc = 0;

    for (int i = 0; i < lines.size(); ++i) {
        lineOffsets[i] = acc;
        acc += lines[i].size() + 1;
    }

    if (plan.detected == SourceType::Code) {
        // Шапка файла как контекст каждого чанка: импорты/инклюды/доку.
        QString header;
        int headerEnd = 0;

        for (int i = 0; i < qMin(25, lines.size()); ++i) {
            const QString &l = lines[i];

            if (l.startsWith(QLatin1String("#include")) ||
                l.startsWith(QLatin1String("import ")) ||
                l.startsWith(QLatin1String("from ")) ||
                l.startsWith(QLatin1String("//")) ||
                l.startsWith(QLatin1String("/*")) ||
                l.startsWith(QLatin1String("*")) ||
                l.trimmed().isEmpty()) {
                header += l;
                header += QLatin1Char('\n');
                headerEnd = lineOffsets[i] + l.size() + 1;
            } else {
                break;
            }
        }
        (void)headerEnd;

        QVector<int> boundaries;

        auto it = codeUnit().globalMatch(text);
        while (it.hasNext()) {
            boundaries.push_back(it.next().capturedStart());
        }

        if (boundaries.isEmpty()) {
            boundaries.push_back(0);
        }

        QVector<QString> units, labels;
        QVector<int> starts, ends;
        QVector<QJsonObject> metas;

        for (int i = 0; i < boundaries.size(); ++i) {
            const int s = boundaries[i];
            const int e = (i + 1 < boundaries.size()) ? boundaries[i + 1] : text.size();

            const QString body = text.mid(s, e - s);

            if (body.trimmed().isEmpty()) {
                continue;
            }

            const QString firstLine = body.section(QLatin1Char('\n'), 0, 0).trimmed();

            units.push_back(header + body);
            labels.push_back(firstLine.left(90));
            starts.push_back(s);
            ends.push_back(e);

            QJsonObject m;
            m.insert(QStringLiteral("path"), sourceUri);
            m.insert(QStringLiteral("language"), languageOf(sourceUri));
            m.insert(QStringLiteral("symbol"), firstLine.left(160));
            m.insert(QStringLiteral("start_line"), text.left(s).count(QLatin1Char('\n')) + 1);
            m.insert(QStringLiteral("end_line"), text.left(e).count(QLatin1Char('\n')) + 1);
            metas.push_back(m);
        }

        plan.chunks = packUnits(units, labels, starts, ends, metas,
                                targetTokenSize, overlapTokens);

        if (plan.chunks.isEmpty()) {
            ChunkDraft d;
            d.text = text.trimmed();
            d.tokenEst = estimateTokens(d.text);
            d.charStart = 0;
            d.charEnd = text.size();
            d.metadata = QJsonObject{{QStringLiteral("path"), sourceUri}};
            plan.chunks.push_back(d);
        }

        return plan;
    }

    // Markdown / notes: режем по заголовкам, держим heading_path.
    QStringList headingStack;
    QVector<QString> units, labels;
    QVector<int> starts, ends;
    QVector<QJsonObject> metas;

    QString cur;
    int curStart = 0;
    QStringList curPath;

    auto closeSection = [&](int endLine) {
        if (cur.trimmed().isEmpty() && curPath.isEmpty()) {
            return;
        }

        const QString joined = curPath.join(QStringLiteral(" › "));

        units.push_back(cur);
        labels.push_back(joined);
        starts.push_back(curStart);
        ends.push_back(lineOffsets.value(qMin(endLine, lineOffsets.size() - 1), text.size()));

        QJsonObject m;
        m.insert(QStringLiteral("heading_path"), QJsonArray::fromStringList(curPath));
        m.insert(QStringLiteral("source_uri"), sourceUri);
        metas.push_back(m);

        cur.clear();
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString &l = lines[i];
        const auto m = mdHeading().match(l);

        if (m.hasMatch()) {
            closeSection(i - 1);

            const int level = m.captured(1).size();
            const QString title = m.captured(2).trimmed();

            while (headingStack.size() >= level) {
                headingStack.removeLast();
            }

            headingStack.push_back(title);
            curPath = headingStack;
            curStart = lineOffsets[i];

            cur += l;
            cur += QLatin1Char('\n');
            continue;
        }

        if (cur.isEmpty() && curPath.isEmpty()) {
            curStart = lineOffsets[i];
        }

        cur += l;
        cur += QLatin1Char('\n');
    }

    closeSection(lines.size() - 1);

    if (units.isEmpty()) {
        ChunkDraft d;
        d.text = text.trimmed();
        d.tokenEst = estimateTokens(d.text);
        d.charStart = 0;
        d.charEnd = text.size();
        plan.chunks.push_back(d);
        return plan;
    }

    plan.chunks = packUnits(units, labels, starts, ends, metas,
                            targetTokenSize, overlapTokens);

    return plan;
}

QString guessTitle(SourceType type, const QString &sourceUri, const QString &text)
{
    // v1: эвристика. Осмысленный title «кратко о содержании» — работа 27b
    // в idle (R4). Здесь честно заглушка: doc-вектор считается от
    // title_эвристика + содержание, основную работу несут чанк-вектора.
    const QStringList lines = text.split(QLatin1Char('\n'));

    for (int i = 0; i < qMin(20, lines.size()); ++i) {
        const QString l = lines[i].trimmed();

        if (l.startsWith(QLatin1String("# "))) {
            return l.mid(2).trimmed();
        }

        if (type == SourceType::Code && !l.isEmpty() &&
            !l.startsWith(QLatin1String("#include")) &&
            !l.startsWith(QLatin1String("import"))) {
            return QStringLiteral("%1 — %2")
                .arg(QFileInfo(sourceUri).fileName(), l.left(70));
        }
    }

    const QString name = QFileInfo(sourceUri).completeBaseName();

    if (!name.isEmpty()) {
        return name;
    }

    return QStringLiteral("untitled");
}

} // namespace rag