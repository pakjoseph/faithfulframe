#include "faithfulframe-deferred-pending.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

constexpr size_t transfer_count = 20'000;

struct pending_node
{
	size_t sequence = 0;
};

void check(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "deferred pending smoke failed at line %d\n", line);
		std::abort();
	}
}

#define CHECK(condition) check((condition), __LINE__)

} // namespace

int main()
{
	faithfulframe_deferred_pending<pending_node> pending;
	auto node = std::make_unique<pending_node>();
	auto other = std::make_unique<pending_node>();
	std::atomic<size_t> stored{0};
	std::atomic<size_t> consumed{0};
	std::atomic<bool> producer_done{false};

	pending_node *const node_raw = node.get();
	CHECK(pending.try_store(node));
	CHECK(!pending.try_store(other));
	CHECK(pending.occupied());
	CHECK(pending.take() == node_raw);
	node.reset(node_raw);
	CHECK(!pending.occupied());
	pending_node *const other_raw = other.get();
	CHECK(pending.try_store(other));
	CHECK(pending.take() == other_raw);
	other.reset(other_raw);

	std::thread producer([&] {
		for (size_t sequence = 0; sequence < transfer_count; ++sequence) {
			while (consumed.load(std::memory_order_acquire) != sequence)
				std::this_thread::yield();
			auto current = std::make_unique<pending_node>();
			current->sequence = sequence;
			while (!pending.try_store(current))
				std::this_thread::yield();
			stored.fetch_add(1, std::memory_order_release);
		}
		producer_done.store(true, std::memory_order_release);
	});

	size_t expected_sequence = 0;
	while (!producer_done.load(std::memory_order_acquire) || pending.occupied()) {
		if (auto taken = std::unique_ptr<pending_node>(pending.take()); taken) {
			CHECK(taken->sequence == expected_sequence);
			++expected_sequence;
			consumed.fetch_add(1, std::memory_order_release);
		} else {
			std::this_thread::yield();
		}
	}
	producer.join();

	CHECK(expected_sequence == transfer_count);
	CHECK(stored.load(std::memory_order_acquire) == transfer_count);
	CHECK(consumed.load(std::memory_order_acquire) == transfer_count);
	return 0;
}
