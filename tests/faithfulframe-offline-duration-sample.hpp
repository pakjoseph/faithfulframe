#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace faithfulframe::offline {

/// Keeps exact initial samples, then uses deterministic reservoir sampling.
/// Storage stays bounded for arbitrarily long streamed detector runs.
class duration_sample {
public:
	static constexpr std::size_t default_capacity = 65'536;

	explicit duration_sample(std::size_t capacity = default_capacity) : capacity_(capacity)
	{
		values_.reserve(capacity_);
	}

	void add(uint64_t value)
	{
		if (values_.size() < capacity_) {
			values_.push_back(value);
		} else if (capacity_ > 0) {
			const uint64_t population = seen_ == std::numeric_limits<uint64_t>::max() ? seen_ : seen_ + 1;
			const uint64_t index = next_random() % population;
			if (index < capacity_)
				values_[static_cast<std::size_t>(index)] = value;
		}
		if (seen_ < std::numeric_limits<uint64_t>::max())
			++seen_;
	}

	bool exact() const noexcept { return seen_ == values_.size(); }
	std::size_t capacity() const noexcept { return capacity_; }
	uint64_t seen() const noexcept { return seen_; }
	void sort() { std::sort(values_.begin(), values_.end()); }
	const std::vector<uint64_t> &values() const noexcept { return values_; }

private:
	uint64_t next_random() noexcept
	{
		// xorshift64* gives a reproducible replacement sequence on every target.
		random_state_ ^= random_state_ >> 12;
		random_state_ ^= random_state_ << 25;
		random_state_ ^= random_state_ >> 27;
		return random_state_ * 0x2545F4914F6CDD1DULL;
	}

	const std::size_t capacity_;
	std::vector<uint64_t> values_;
	uint64_t seen_ = 0;
	uint64_t random_state_ = 0xD1B54A32D192ED03ULL;
};

} // namespace faithfulframe::offline
