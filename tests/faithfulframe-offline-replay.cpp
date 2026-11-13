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

#include "faithfulframe-offline-frame-sequence.hpp"
#include "faithfulframe-offline-duration-sample.hpp"

#include <obs-module.h>
#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/serialize.h>
#include "faithfulframe-worker-log.hpp"
#include "face-detector-dlib-hog.h"
#include "texture-object.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using steady_clock = std::chrono::steady_clock;
using faithfulframe::offline::frame_spec;
using faithfulframe::offline::rgb_frame;

struct process_sample
{
	clock_t cpu_time = static_cast<clock_t>(-1);
	uint64_t resident_bytes = 0;
	uint64_t peak_resident_bytes = 0;
	bool memory_valid = false;
};

process_sample sample_process()
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

bool elapsed_cpu_microseconds(clock_t begin, clock_t end, uint64_t &microseconds)
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

bool signed_difference(uint64_t end, uint64_t start, int64_t &difference)
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

void print_usage(std::ostream &output)
{
	output << "Usage: faithfulframe-offline-replay --model <frontal_face_detector.dat> "
		  "(--manifest <frames.tsv> | --ppm-stdin [--fps <1..120>]) [--csv <output.csv>]\n"
	       << "Manifest format: monotonic_ms<TAB>relative-or-absolute-frame-path\n"
	       << "Frames must be binary P6 PPM files with max value 255. Use one file per manifest entry, or "
		  "concatenate frames on stdin.\n"
	       << "This tool benchmarks the production dlib HOG detector only; it does not validate OBS capture, "
		  "target selection, or tracking policy. Frames are processed serially as fast as detection completes; "
		  "manifest timestamps are recorded but do not pace replay. In PPM stdin mode, timestamps are synthesized "
		  "from --fps (default 5), which does not pace replay. Completion latency includes up to 1 ms polling delay. "
		  "Detector latency percentiles use exact samples through 65536 frames, then a deterministic reservoir "
		  "sample with fixed memory use. "
		  "CSV rows include process CPU and resident/peak memory samples; CPU time uses std::clock and memory "
		  "sampling is platform-specific. process_cpu_percent is process CPU time divided by wall time, where 100% "
		  "equals one logical CPU and multithreaded use can exceed 100%. These process-wide measurements are "
		  "diagnostic, not production acceptance. "
		  "The HOG score is dlib's raw detector margin, not a probability or calibrated confidence.\n";
}

bool parse_arguments(int argc, char **argv, std::filesystem::path &model_path, std::filesystem::path &manifest_path,
		     std::filesystem::path &csv_path, bool &ppm_stdin, uint32_t &fps)
{
	for (int i = 1; i < argc; ++i) {
		const std::string option(argv[i]);
		if (option == "--help" || option == "-h") {
			print_usage(std::cout);
			return false;
		}
		if (option == "--ppm-stdin") {
			ppm_stdin = true;
			continue;
		}
		if (i + 1 >= argc) {
			std::cerr << "Missing value for " << option << '\n';
			return false;
		}
		const std::string value(argv[++i]);
		if (option == "--model")
			model_path = value;
		else if (option == "--manifest")
			manifest_path = value;
		else if (option == "--csv")
			csv_path = value;
		else if (option == "--fps") {
			uint32_t parsed_fps = 0;
			const char *begin = value.data();
			const char *end = begin + value.size();
			const auto parsed = std::from_chars(begin, end, parsed_fps, 10);
			if (parsed.ec != std::errc{} || parsed.ptr != end || parsed_fps < 1 || parsed_fps > 120) {
				std::cerr << "--fps must be an integer from 1 through 120\n";
				return false;
			}
			fps = parsed_fps;
		} else {
			std::cerr << "Unknown option: " << option << '\n';
			return false;
		}
	}
	if (model_path.empty() || ppm_stdin == !manifest_path.empty()) {
		print_usage(std::cerr);
		return false;
	}
	return true;
}

bool stream_timestamp_ms(uint64_t frame_index, uint32_t fps, uint64_t &timestamp_ms)
{
	const uint64_t whole_seconds = frame_index / fps;
	const uint64_t fractional_ms = (frame_index % fps) * 1000 / fps;
	if (whole_seconds > std::numeric_limits<uint64_t>::max() / 1000)
		return false;
	const uint64_t whole_ms = whole_seconds * 1000;
	if (fractional_ms > std::numeric_limits<uint64_t>::max() - whole_ms)
		return false;
	timestamp_ms = whole_ms + fractional_ms;
	return true;
}

bool load_frame(const frame_spec &spec, rgb_frame &frame, uint64_t &decode_us, std::string &error)
{
	const auto started = steady_clock::now();
	std::ifstream input(spec.path, std::ios::binary);
	if (!input) {
		error = "cannot open frame: " + spec.path.string();
		return false;
	}
	if (!faithfulframe::offline::read_ppm(input, frame, error)) {
		error = spec.path.string() + ": " + error;
		return false;
	}
	decode_us = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(steady_clock::now() - started).count());
	return true;
}

bool validate_model(const std::filesystem::path &path, std::string &error)
{
	try {
		dlib::frontal_face_detector detector;
		dlib::deserialize(path.string()) >> detector;
	} catch (const std::exception &exception) {
		error = "cannot deserialize dlib HOG model: " + std::string(exception.what());
		return false;
	} catch (...) {
		error = "cannot deserialize dlib HOG model: unknown error";
		return false;
	}
	return true;
}

std::shared_ptr<texture_object> make_texture(rgb_frame &image, uint64_t timestamp_ms)
{
	if (image.width > std::numeric_limits<uint32_t>::max() / 3)
		return nullptr;
	const size_t stride = static_cast<size_t>(image.width) * 3;
	for (size_t i = 0; i < image.pixels.size(); i += 3) {
		std::swap(image.pixels[i], image.pixels[i + 2]);
	}
	obs_source_frame source_frame{};
	source_frame.data[0] = image.pixels.data();
	source_frame.linesize[0] = static_cast<uint32_t>(stride);
	source_frame.width = image.width;
	source_frame.height = image.height;
	source_frame.format = VIDEO_FORMAT_BGR3;
	auto texture = std::make_shared<texture_object>();
	texture->scale = 1.0f;
	texture->captured_at_ms = timestamp_ms;
	texture->detection_epoch = 1;
	texture->set_texture_obsframe(&source_frame, 1);
	return texture;
}

bool wait_for_completion(face_detector_dlib_hog &detector, uint64_t expected_count, uint64_t &latency_us)
{
	const auto started = steady_clock::now();
	const auto deadline = started + std::chrono::minutes(2);
	while (detector.completed_work_count() < expected_count) {
		if (detector.has_failed())
			return false;
		if (steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	latency_us = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(steady_clock::now() - started).count());
	return true;
}

std::string csv_path(const std::filesystem::path &path)
{
	std::string escaped;
	escaped.reserve(path.string().size() + 2);
	escaped.push_back('"');
	for (char c : path.string()) {
		if (c == '"')
			escaped.push_back('"');
		escaped.push_back(c);
	}
	escaped.push_back('"');
	return escaped;
}

bool same_file_or_path(const std::filesystem::path &left, const std::filesystem::path &right)
{
	if (left.lexically_normal() == right.lexically_normal())
		return true;
	std::error_code error;
	const auto left_canonical = std::filesystem::weakly_canonical(left, error);
	if (error)
		return false;
	const auto right_canonical = std::filesystem::weakly_canonical(right, error);
	return !error && left_canonical == right_canonical;
}

} // namespace

int main(int argc, char **argv)
{
	std::filesystem::path model_path;
	std::filesystem::path manifest_path;
	std::filesystem::path csv_path_value;
	bool ppm_stdin = false;
	uint32_t fps = 5;
	if (!parse_arguments(argc, argv, model_path, manifest_path, csv_path_value, ppm_stdin, fps))
		return argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") ? 0 : 2;
	if (!std::filesystem::is_regular_file(model_path)) {
		std::cerr << "Model file does not exist: " << model_path << '\n';
		return 2;
	}
	std::string error;
	if (!validate_model(model_path, error)) {
		std::cerr << error << '\n';
		return 2;
	}
	std::vector<frame_spec> frames;
	if (!ppm_stdin) {
		std::ifstream manifest_input(manifest_path);
		if (!manifest_input) {
			std::cerr << "Cannot open manifest: " << manifest_path << '\n';
			return 2;
		}
		if (!faithfulframe::offline::read_manifest(manifest_input, manifest_path.parent_path(), frames,
							   error)) {
			std::cerr << error << '\n';
			return 2;
		}
	} else {
#ifdef _WIN32
		if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
			std::cerr << "Could not switch stdin to binary mode\n";
			return 2;
		}
#endif
	}
	if (!csv_path_value.empty()) {
		if (same_file_or_path(csv_path_value, model_path) ||
		    (!ppm_stdin && same_file_or_path(csv_path_value, manifest_path))) {
			std::cerr << "CSV output must not replace the model or frame manifest\n";
			return 2;
		}
		for (const frame_spec &frame : frames) {
			if (same_file_or_path(csv_path_value, frame.path)) {
				std::cerr << "CSV output must not replace an input frame: " << frame.path << '\n';
				return 2;
			}
		}
	}
	std::ofstream csv_file;
	std::ostream *csv = &std::cout;
	if (!csv_path_value.empty()) {
		csv_file.open(csv_path_value, std::ios::out | std::ios::trunc);
		if (!csv_file) {
			std::cerr << "Cannot create CSV output: " << csv_path_value << '\n';
			return 2;
		}
		csv = &csv_file;
	}
	*csv << "frame_index,monotonic_ms,frame_path,width,height,decode_us,completion_wait_us,"
		"detector_work_us,process_cpu_us,process_cpu_percent,cpu_valid,resident_bytes,peak_resident_bytes,"
		"memory_valid,detection_index,x0,y0,x1,y1,score\n";
	if (!*csv) {
		std::cerr << "Failed while writing CSV header\n";
		return 2;
	}

	face_detector_dlib_hog detector;
	detector.set_model(model_path.string().c_str());
	const size_t sample_capacity =
		ppm_stdin ? faithfulframe::offline::duration_sample::default_capacity
			  : std::min(frames.size(), faithfulframe::offline::duration_sample::default_capacity);
	faithfulframe::offline::duration_sample work_durations(sample_capacity);
	uint64_t total_work_us = 0;
	uint64_t max_work_us = 0;
	bool total_work_valid = true;
	const auto replay_started = steady_clock::now();
	const process_sample process_started = sample_process();
	if (!detector.start()) {
		std::cerr << "Could not start the dlib detector worker\n";
		return 2;
	}
	uint64_t total_detections = 0;
	uint64_t frame_index = 0;
	size_t manifest_index = 0;
	while (true) {
		frame_spec current_frame;
		rgb_frame image;
		uint64_t decode_us = 0;
		if (ppm_stdin) {
			const auto decode_started = steady_clock::now();
			const auto result = faithfulframe::offline::read_ppm_stream_frame(std::cin, image, error);
			decode_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
								  steady_clock::now() - decode_started)
								  .count());
			if (result == faithfulframe::offline::ppm_stream_result::end_of_stream)
				break;
			if (result == faithfulframe::offline::ppm_stream_result::error) {
				std::cerr << error << '\n';
				return 2;
			}
			if (!stream_timestamp_ms(frame_index, fps, current_frame.monotonic_ms)) {
				std::cerr << "Frame count exceeded the supported timestamp range\n";
				return 2;
			}
			current_frame.path = "stdin";
		} else {
			if (manifest_index >= frames.size())
				break;
			current_frame = frames[manifest_index++];
			if (!load_frame(current_frame, image, decode_us, error)) {
				std::cerr << error << '\n';
				return 2;
			}
		}
		if (image.width < 80 || image.height < 80) {
			std::cerr << "dlib HOG requires frames of at least 80x80 pixels: " << current_frame.path
				  << '\n';
			return 2;
		}
		auto texture = make_texture(image, current_frame.monotonic_ms);
		if (!texture) {
			std::cerr << "Could not prepare frame " << current_frame.path << '\n';
			return 2;
		}
		const uint64_t expected_count = detector.completed_work_count() + 1;
		detector.lock();
		detector.set_texture(texture, 0, 0, 0, 0);
		detector.signal();
		detector.unlock();
		uint64_t queue_latency_us = 0;
		if (!wait_for_completion(detector, expected_count, queue_latency_us) || detector.has_failed()) {
			std::cerr << "Detector failed or timed out on frame " << frame_index << '\n';
			return 3;
		}
		std::vector<rect_s> detections;
		detector.lock();
		detector.get_faces(detections);
		detector.unlock();
		const uint64_t detector_work_us = detector.last_work_duration_us();
		const process_sample current_process = sample_process();
		max_work_us = std::max(max_work_us, detector_work_us);
		uint64_t process_cpu_us = 0;
		const bool cpu_valid =
			elapsed_cpu_microseconds(process_started.cpu_time, current_process.cpu_time, process_cpu_us);
		const uint64_t wall_elapsed_us = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(steady_clock::now() - replay_started)
				.count());
		const double process_cpu_percent =
			cpu_valid && wall_elapsed_us > 0
				? 100.0 * static_cast<double>(process_cpu_us) / static_cast<double>(wall_elapsed_us)
				: 0.0;
		work_durations.add(detector_work_us);
		if (detector_work_us > std::numeric_limits<uint64_t>::max() - total_work_us)
			total_work_valid = false;
		else if (total_work_valid)
			total_work_us += detector_work_us;
		total_detections += detections.size();
		if (detections.empty()) {
			*csv << frame_index << ',' << current_frame.monotonic_ms << ',' << csv_path(current_frame.path)
			     << ',' << image.width << ',' << image.height << ',' << decode_us << ',' << queue_latency_us
			     << ',' << detector_work_us << ','
			     << (cpu_valid ? std::to_string(process_cpu_us) : std::string{}) << ','
			     << (cpu_valid ? std::to_string(process_cpu_percent) : std::string{}) << ','
			     << (cpu_valid ? 1 : 0) << ','
			     << (current_process.memory_valid ? std::to_string(current_process.resident_bytes)
							      : std::string{})
			     << ','
			     << (current_process.memory_valid ? std::to_string(current_process.peak_resident_bytes)
							      : std::string{})
			     << ',' << (current_process.memory_valid ? 1 : 0) << ",,,,,,\n";
		} else {
			for (size_t detection_index = 0; detection_index < detections.size(); ++detection_index) {
				const rect_s &rect = detections[detection_index];
				*csv << frame_index << ',' << current_frame.monotonic_ms << ','
				     << csv_path(current_frame.path) << ',' << image.width << ',' << image.height << ','
				     << decode_us << ',' << queue_latency_us << ',' << detector_work_us << ','
				     << (cpu_valid ? std::to_string(process_cpu_us) : std::string{}) << ','
				     << (cpu_valid ? std::to_string(process_cpu_percent) : std::string{}) << ','
				     << (cpu_valid ? 1 : 0) << ','
				     << (current_process.memory_valid ? std::to_string(current_process.resident_bytes)
								      : std::string{})
				     << ','
				     << (current_process.memory_valid
						 ? std::to_string(current_process.peak_resident_bytes)
						 : std::string{})
				     << ',' << (current_process.memory_valid ? 1 : 0) << ',' << detection_index << ','
				     << rect.x0 << ',' << rect.y0 << ',' << rect.x1 << ',' << rect.y1 << ','
				     << std::setprecision(6) << rect.score << '\n';
			}
		}
		if (!*csv) {
			std::cerr << "Failed while writing detector output\n";
			return 2;
		}
		++frame_index;
	}
	detector.stop();
	if (frame_index == 0) {
		std::cerr << (ppm_stdin ? "PPM stdin contains no frames\n" : "Manifest contains no frames\n");
		return 2;
	}
	const process_sample process_finished = sample_process();
	work_durations.sort();
	const auto &duration_values = work_durations.values();
	const auto percentile = [&duration_values](double fraction) {
		const size_t index = static_cast<size_t>(fraction * static_cast<double>(duration_values.size() - 1));
		return duration_values[index];
	};
	const uint64_t replay_wall_us = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(steady_clock::now() - replay_started).count());
	uint64_t total_process_cpu_us = 0;
	const bool total_cpu_valid =
		elapsed_cpu_microseconds(process_started.cpu_time, process_finished.cpu_time, total_process_cpu_us);
	std::cerr << "frames=" << frame_index << " detections=" << total_detections
		  << " replay_wall_us=" << replay_wall_us << " detector_mean_us=";
	if (total_work_valid)
		std::cerr << (total_work_us / frame_index);
	else
		std::cerr << "unavailable";
	std::cerr << " detector_p50_us=" << percentile(0.50) << " detector_p95_us=" << percentile(0.95)
		  << " detector_max_us=" << max_work_us << " detector_percentile_samples=" << duration_values.size()
		  << " detector_percentiles_exact=" << (work_durations.exact() ? 1 : 0);
	if (total_cpu_valid && replay_wall_us > 0)
		std::cerr << " process_cpu_us=" << total_process_cpu_us << " process_cpu_percent="
			  << 100.0 * static_cast<double>(total_process_cpu_us) / static_cast<double>(replay_wall_us);
	if (process_finished.memory_valid)
		std::cerr << " resident_bytes=" << process_finished.resident_bytes
			  << " peak_resident_bytes=" << process_finished.peak_resident_bytes;
	int64_t resident_growth_bytes = 0;
	if (process_started.memory_valid && process_finished.memory_valid &&
	    signed_difference(process_finished.resident_bytes, process_started.resident_bytes, resident_growth_bytes))
		std::cerr << " resident_start_bytes=" << process_started.resident_bytes
			  << " resident_growth_bytes=" << resident_growth_bytes;
	std::cerr << '\n';
	csv->flush();
	if (!*csv) {
		std::cerr << "Failed while flushing detector output\n";
		return 2;
	}
	return 0;
}
