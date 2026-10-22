#pragma once

#include "rag_types.h"

namespace rag {

struct ChunkPlan {
    QVector<ChunkDraft> chunks;
    SourceType detected = SourceType::Unknown;
    QStringList warnings;
};

// targetTokenSize ~450: пара чанков = ~900 токенов, что влезает
// в 2000-лимит входа для 27b-судьи вместе с thinking.
ChunkPlan chunkDocument(
    SourceType hint,
    const QString &sourceUri,
    const QString &text,
    int targetTokenSize = 450,
    int overlapTokens = 60
);

QString guessTitle(SourceType type, const QString &sourceUri, const QString &text);

} // namespace rag