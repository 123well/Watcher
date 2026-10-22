#pragma once

#include <QString>
#include <QByteArray>

#include <vector>

namespace rag {

// Грубая, но стабильная оценка: ~3.5 UTF-16 единицы на токен для смеси RU/EN/кода.
// Только для бюджетов, не для точного подсчёта.
int estimateTokens(const QString &text);

QString sha256Hex(const QByteArray &data);

// Нормализация для hash: схлопываем пробелы, убираем \r, не трогаем регистр
// (иначе потеряем различие File/file).
QString normalizeForHash(const QString &text);

std::vector<float> normalizeVector(std::vector<float> v);
float dot(const std::vector<float> &a, const std::vector<float> &b);

QString nowIso();
double daysSince(const QString &iso);

QString truncateUtf8(const QString &s, int maxChars, bool *wasTruncated);

} // namespace rag