#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <limits>

namespace faithfulframe::offline {

struct process_sample
{
	clock_t cpu_time = static_cast<clock_t>(-1);
	uint64_t resident_bytes = 0;
	uint64_t peak_resident_bytes = 0;
	bool memory_valid = false;
};

inline process_sample sample_process()
{
	process_sample sample;
	bool resident_valid = false;
	bool peak_resident_valid = false;
	sample.cpu_time = std::clock();
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
				 sizeof(counters))) {
		sample.resident_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
		sample.peak_resident_bytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
		resident_valid = true;
		peak_resident_valid = true;
	}
#elif defined(__APPLE__)
	mach_task_basic_info_data_t task_info_data{};
	mach_msg_type_number_t task_info_count = MACH_TASK_BASIC_INFO_COUNT;
	const bool current_rss_available = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
						     reinterpret_cast<task_info_t>(&task_info_data),
						     &task_info_count) == KERN_SUCCESS;
	struct rusage usage{};
	const bool peak_valid = getrusage(RUSAGE_SELF, &usage) == 0;
	if (current_rss_available) {
		sample.resident_bytes = static_cast<uint64_t>(task_info_data.resident_size);
		resident_valid = true;
	}
	if (peak_valid && usage.ru_maxrss >= 0) {
		sample.peak_resident_bytes = static_cast<uint64_t>(usage.ru_maxrss);
		peak_resident_valid = true;
	}
#else
	struct rusage usage{};
	const bool peak_valid = getrusage(RUSAGE_SELF, &usage) == 0;
	if (peak_valid && usage.ru_maxrss >= 0 &&
	    static_cast<uint64_t>(usage.ru_maxrss) <= std::numeric_limits<uint64_t>::max() / 1024) {
		sample.peak_resident_bytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
		peak_resident_valid = true;
	}
	std::ifstream statm("/proc/self/statm");
	uint64_t total_pages = 0;
	uint64_t resident_pages = 0;
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size > 0 && statm >> total_pages >> resident_pages &&
	    resident_pages <= std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(page_size)) {
		sample.resident_bytes = resident_pages * static_cast<uint64_t>(page_size);
		resident_valid = true;
	}
#endif
	sample.memory_valid = resident_valid && peak_resident_valid;
	if (sample.memory_valid)
		sample.peak_resident_bytes = std::max(sample.peak_resident_bytes, sample.resident_bytes);
	return sample;
}

inline bool elapsed_cpu_microseconds(clock_t begin, clock_t end, uint64_t &microseconds)
{
	if (begin == static_cast<clock_t>(-1) || end == static_cast<clock_t>(-1) || end < begin || CLOCKS_PER_SEC <= 0)
		return false;
	const long double elapsed =
		static_cast<long double>(end - begin) * 1000000.0L / static_cast<long double>(CLOCKS_PER_SEC);
	if (elapsed < 0.0L || elapsed > static_cast<long double>(std::numeric_limits<uint64_t>::max()))
		return false;
	microseconds = static_cast<uint64_t>(elapsed);
	return true;
}

inline bool signed_difference(uint64_t end, uint64_t start, int64_t &difference)
{
	if (end >= start) {
		const uint64_t growth = end - start;
		if (growth > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			return false;
		difference = static_cast<int64_t>(growth);
		return true;
	}
	const uint64_t reduction = start - end;
	const uint64_t minimum_magnitude = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
	if (reduction > minimum_magnitude)
		return false;
	difference = reduction == minimum_magnitude ? std::numeric_limits<int64_t>::min()
						    : -static_cast<int64_t>(reduction);
	return true;
}

} // namespace faithfulframe::offline
