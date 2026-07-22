#pragma once

#include <QDateTime>
#include <QString>

#include <stdint.h>

[[nodiscard]] QString formatByteCount(uint64_t bytes);
[[nodiscard]] QString formatSnapshotTime(const QDateTime& utcTime);
