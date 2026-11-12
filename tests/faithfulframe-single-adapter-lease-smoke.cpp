#include "faithfulframe-adapter-lease.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

int main()
{
	int first_owner = 0;
	int second_owner = 0;
	if (faithfulframe::single_adapter_lease::try_acquire(nullptr) ||
	    !faithfulframe::single_adapter_lease::try_acquire(&first_owner) ||
	    !faithfulframe::single_adapter_lease::owns(&first_owner) ||
	    !faithfulframe::single_adapter_lease::try_acquire(&first_owner) ||
	    faithfulframe::single_adapter_lease::try_acquire(&second_owner))
		return 1;
	faithfulframe::single_adapter_lease::release(&second_owner);
	if (!faithfulframe::single_adapter_lease::owns(&first_owner))
		return 2;
	faithfulframe::single_adapter_lease::release(&first_owner);
	if (faithfulframe::single_adapter_lease::owns(&first_owner) ||
	    !faithfulframe::single_adapter_lease::try_acquire(&second_owner))
		return 3;
	faithfulframe::single_adapter_lease::release(&second_owner);

	constexpr std::size_t owner_count = 16;
	std::array<int, owner_count> owners{};
	std::array<bool, owner_count> acquired{};
	std::atomic<bool> start{false};
	std::atomic<std::size_t> winners{0};
	std::vector<std::thread> contenders;
	contenders.reserve(owner_count);
	for (std::size_t i = 0; i < owner_count; ++i) {
		contenders.emplace_back([&, i] {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			acquired[i] = faithfulframe::single_adapter_lease::try_acquire(&owners[i]);
			if (acquired[i])
				winners.fetch_add(1, std::memory_order_relaxed);
		});
	}
	start.store(true, std::memory_order_release);
	for (std::thread &contender : contenders)
		contender.join();
	if (winners.load(std::memory_order_relaxed) != 1)
		return 4;
	for (std::size_t i = 0; i < owner_count; ++i) {
		if (acquired[i] != faithfulframe::single_adapter_lease::owns(&owners[i]))
			return 5;
		if (acquired[i])
			faithfulframe::single_adapter_lease::release(&owners[i]);
	}
	return 0;
}
