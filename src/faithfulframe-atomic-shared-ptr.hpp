#pragma once

#include <atomic>
#include <memory>
#include <utility>

// C++20 deprecates the free atomic shared_ptr functions. They remain the
// portable implementation for the Apple libc++ used by our macOS deployment
// toolchain, which does not yet provide std::atomic<std::shared_ptr<T>>.
// Keep the deprecation exception limited to these compatibility wrappers so
// other deprecated APIs remain errors in CI.
namespace faithfulframe {

template<typename T>
std::shared_ptr<T> atomic_shared_ptr_load(const std::shared_ptr<T> *value, std::memory_order order) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	auto result = std::atomic_load_explicit(value, order);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
	return result;
}

template<typename T>
void atomic_shared_ptr_store(std::shared_ptr<T> *value, std::shared_ptr<T> desired, std::memory_order order) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	std::atomic_store_explicit(value, std::move(desired), order);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

template<typename T>
std::shared_ptr<T> atomic_shared_ptr_exchange(std::shared_ptr<T> *value, std::shared_ptr<T> desired,
					      std::memory_order order) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	auto result = std::atomic_exchange_explicit(value, std::move(desired), order);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
	return result;
}

} // namespace faithfulframe
