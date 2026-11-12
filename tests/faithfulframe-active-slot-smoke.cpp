#include "faithfulframe-active-slot.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <thread>

struct test_binding
{
	std::atomic<bool> *destroyed;
	~test_binding()
	{
		if (destroyed)
			destroyed->store(true, std::memory_order_release);
	}
};

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "active binding slot check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main()
{
	faithfulframe_active_slot<test_binding> slot;
	std::atomic<bool> old_destroyed{false};
	std::atomic<bool> reader_entered{false};
	std::atomic<bool> release_reader{false};
	std::atomic<bool> drain_waiter_entered{false};
	std::atomic<bool> drained{false};
	auto old_binding = std::make_unique<test_binding>(&old_destroyed);
	auto new_binding = std::make_unique<test_binding>(nullptr);
	test_binding *const old_pointer = old_binding.get();
	test_binding *const new_pointer = new_binding.get();
	slot.store(old_pointer);

	std::thread reader([&] {
		auto lease = slot.read();
		check(lease.get() == old_pointer);
		reader_entered.store(true, std::memory_order_release);
		while (!release_reader.load(std::memory_order_acquire))
			std::this_thread::yield();
		check(lease.get() == old_pointer);
		check(!old_destroyed.load(std::memory_order_acquire));
	});
	while (!reader_entered.load(std::memory_order_acquire))
		std::this_thread::yield();

	check(slot.exchange(nullptr) == old_pointer);
	slot.store(new_pointer);
	check(slot.reader_count() == 1);
	check(!old_destroyed.load(std::memory_order_acquire));
	{
		auto current = slot.read();
		check(current.get() == new_pointer);
	}

	std::thread drain_waiter([&] {
		drain_waiter_entered.store(true, std::memory_order_release);
		slot.wait_until_drained();
		drained.store(true, std::memory_order_release);
	});
	while (!drain_waiter_entered.load(std::memory_order_acquire))
		std::this_thread::yield();
	check(!drained.load(std::memory_order_acquire));
	release_reader.store(true, std::memory_order_release);
	reader.join();
	drain_waiter.join();
	check(drained.load(std::memory_order_acquire));
	check(slot.reader_count() == 0);
	old_binding.reset();
	check(old_destroyed.load(std::memory_order_acquire));
	check(slot.peek() == new_pointer);
	return 0;
}
