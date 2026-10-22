#include "rag_util.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>

#include <cmath>

namespace rag {

int estimateTokens(const QString &text)
{
    if (text.isEmpty()) {
        return 0;
    }

    // 35/10 = 3.5 символа на токен, +4 на служебные.
    return qMax(1, text.size() * 10 / 35 + 4);
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()
    );
}

QString normalizeForHash(const QString &text)
{
    static const QRegularExpression ws(QStringLiteral("[ \\t\\x0B\\f\\r]+"));
    static const QRegularExpression nl(QStringLiteral("\\n{3,}"));

    QString s = text;
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    s.replace(ws, QStringLiteral(" "));
    s.replace(nl, QStringLiteral("\n\n"));
    return s.trimmed();
}

std::vector<float> normalizeVector(std::vector<float> v)
{
    double sum = 0.0;

    for (float x : v) {
        sum += double(x) * double(x);
    }

    const double norm = std::sqrt(sum);

    if (norm > 1e-9) {
        for (float &x : v) {
            x = float(double(x) / norm);
        }
    }

    return v;
}

float dot(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size()) {
        return 0.f;
    }

    double sum = 0.0;
    const size_t n = a.size();

    for (size_t i = 0; i < n; ++i) {
        sum += double(a[i]) * double(b[i]);
    }

    return float(sum);
}

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

double daysSince(const QString &iso)
{
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);

    if (!dt.isValid()) {
        return 1e9;
    }

    return double(dt.msecsTo(QDateTime::currentDateTimeUtc())) / 86400000.0;
}

QString truncateUtf8(const QString &s, int maxChars, bool *wasTruncated)
{
    if (s.size() <= maxChars) {
        if (wasTruncated) *wasTruncated = false;
        return s;
    }

    if (wasTruncated) *wasTruncated = true;
    return s.left(maxChars) + QStringLiteral("…");
}

} // namespace rag