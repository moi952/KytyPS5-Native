#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_MAPPEDCOUNT_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_MAPPEDCOUNT_H_

#include <cstdint>
#include <iterator>
#include <map>

// How many views cover each byte of a linear space, as a step function: the value of a key holds from that key
// up to the next one; below the first key and past the last change it is 0. PhysicalMemory keeps one over the
// direct-memory offsets of its mappings, so "is this physical range still mapped by another view" is a lookup
// in the range instead of a walk over every mapping (Marvel's Wolverine unmaps automatic memory constantly).
namespace Libs::LibKernel::Memory {

using MappedCount = std::map<uint64_t, uint32_t>;

// Adds `delta` (+1 a view mapped, -1 a view unmapped) over [start, start + size).
inline void MappedCountAdjust(MappedCount& steps, uint64_t start, uint64_t size, int delta) {
	if (size == 0 || delta == 0) {
		return;
	}
	const auto split = [&steps](uint64_t at) {
		auto it = steps.lower_bound(at);
		if (it != steps.end() && it->first == at) {
			return it;
		}
		const uint32_t value = (it == steps.begin() ? 0u : std::prev(it)->second);
		return steps.emplace_hint(it, at, value);
	};
	const auto last  = split(start + size); // keeps the value past the range
	const auto first = split(start);
	for (auto it = first; it != last; ++it) {
		if (delta > 0) {
			it->second += static_cast<uint32_t>(delta);
		} else {
			const auto down = static_cast<uint32_t>(-delta);
			it->second      = (it->second > down ? it->second - down : 0u);
		}
	}
	// Drop the keys that no longer change the value, at both ends and inside.
	const auto stop = std::next(last);
	for (auto it = first; it != stop;) {
		const uint32_t before = (it == steps.begin() ? 0u : std::prev(it)->second);
		it                    = (it->second == before ? steps.erase(it) : std::next(it));
	}
}

// Calls fn(piece_start, piece_size, count) for the consecutive pieces of [start, start + size) with one count each.
template <class Fn>
inline void MappedCountForEach(const MappedCount& steps, uint64_t start, uint64_t size, Fn&& fn) {
	const auto end   = start + size;
	auto       next  = steps.upper_bound(start);
	uint32_t   count = (next == steps.begin() ? 0u : std::prev(next)->second);
	for (auto pos = start; pos < end;) {
		const auto piece_end = (next == steps.end() || next->first > end ? end : next->first);
		if (piece_end > pos) {
			fn(pos, piece_end - pos, count);
		}
		pos = piece_end;
		if (next != steps.end() && pos < end) {
			count = next->second;
			++next;
		}
	}
}

} // namespace Libs::LibKernel::Memory

#endif
