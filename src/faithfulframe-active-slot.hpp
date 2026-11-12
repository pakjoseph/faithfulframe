#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

/// Lock-free reader pinning for a replaceable raw pointer. A writer may reclaim
/// a retired object only after it has unpublished the pointer and the reader
/// count reaches zero. Readers increment before loading the pointer, so a
/// reader that could observe the old value is visible to the writer.
template<typename T> class faithfulframe_active_slot {
public:
	class read_lease {
	public:
		explicit read_lease(faithfulframe_active_slot &slot) : owner(&slot)
		{
			owner->readers.fetch_add(1, std::memory_order_seq_cst);
			value = owner->active.load(std::memory_order_seq_cst);
		}

		~read_lease() { release(); }

		read_lease(const read_lease &) = delete;
		read_lease &operator=(const read_lease &) = delete;

		read_lease(read_lease &&other) noexcept : owner(other.owner), value(other.value)
		{
			other.owner = nullptr;
			other.value = nullptr;
		}

		read_lease &operator=(read_lease &&other) noexcept
		{
			if (this == &other)
				return *this;
			release();
			owner = other.owner;
			value = other.value;
			other.owner = nullptr;
			other.value = nullptr;
			return *this;
		}

		T *get() const { return value; }
		explicit operator bool() const { return value != nullptr; }

	private:
		void release()
		{
			if (owner)
				owner->readers.fetch_sub(1, std::memory_order_seq_cst);
			owner = nullptr;
			value = nullptr;
		}

		faithfulframe_active_slot *owner;
		T *value = nullptr;
	};

	faithfulframe_active_slot() = default;
	faithfulframe_active_slot(const faithfulframe_active_slot &) = delete;
	faithfulframe_active_slot &operator=(const faithfulframe_active_slot &) = delete;

	read_lease read() { return read_lease(*this); }

	T *exchange(T *next) { return active.exchange(next, std::memory_order_seq_cst); }

	void store(T *next) { active.store(next, std::memory_order_seq_cst); }

	/// Non-owning inspection for a writer that holds its object-registry lock.
	T *peek() const { return active.load(std::memory_order_seq_cst); }

	uint64_t reader_count() const { return readers.load(std::memory_order_seq_cst); }

	void wait_until_drained()
	{
		while (readers.load(std::memory_order_seq_cst) != 0)
			std::this_thread::yield();
	}

private:
	static_assert(std::atomic<T *>::is_always_lock_free, "OBS callback pointer loads must remain lock-free");
	static_assert(std::atomic<uint64_t>::is_always_lock_free, "OBS callback reader pins must remain lock-free");

	std::atomic<T *> active{nullptr};
	std::atomic<uint64_t> readers{0};
};
