#pragma once

#include "filesystem_types.hpp"

#include <limits>
#include <optional>
#include <stdint.h>

namespace SnapshotInternal {

struct EntryIdentityLess
{
	[[nodiscard]] bool operator()(const thin_io::entry_identity& left, const thin_io::entry_identity& right) const
	{
		return left.filesystem != right.filesystem ? left.filesystem < right.filesystem : left.entry < right.entry;
	}
};

inline std::optional<uint64_t> addAllocatedSizes(
	const std::optional<uint64_t> total, const std::optional<uint64_t> value, bool& overflow)
{
	if (!total || !value)
		return {};
	if (*value > std::numeric_limits<uint64_t>::max() - *total)
	{
		overflow = true;
		return {};
	}
	return *total + *value;
}

} // namespace SnapshotInternal
