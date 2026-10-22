#include "embedding_provider.h"

namespace rag {

Pooling poolingFromString(const QString &s)
{
    if (s == QLatin1String("mean")) return Pooling::Mean;
    if (s == QLatin1String("last")) return Pooling::Last;
    return Pooling::Cls;   // дефолт: bge-m3 = CLS
}

QString poolingToString(Pooling p)
{
    switch (p) {
        case Pooling::Mean: return QStringLiteral("mean");
        case Pooling::Last: return QStringLiteral("last");
        default:            return QStringLiteral("cls");
    }
}

} // namespace rag
