#include "faithfulframe-offline-frame-sequence.hpp"

#include <obs-module.h>
#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/serialize.h>
#include "faithfulframe-worker-log.hpp"
#include "face-detector-dlib-hog.h"
#include "texture-object.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using steady_clock = std::chrono::steady_clock;
using faithfulframe::offline::frame_spec;
using faithfulframe::offline::rgb_frame;

void print_usage(std::ostream &output)
{
	output << "Usage: faithfulframe-offline-replay --model <frontal_face_detector.dat> --manifest <frames.tsv> "
		  "[--csv <output.csv>]\n"
	       << "Manifest format: monotonic_ms<TAB>relative-or-absolute-frame-path\n"
	       << "Frames must be binary P6 PPM files with max value 255. One image per file.\n"
	       << "This tool benchmarks the production dlib HOG detector only; it does not validate OBS capture, "
		  "target selection, or tracking policy. Frames are processed serially as fast as detection completes; "
		  "timestamps are recorded but do not pace replay. Completion latency includes up to 1 ms polling delay.\n";
}

bool parse_arguments(int argc, char **argv, std::filesystem::path &model_path, std::filesystem::path &manifest_path,
		     std::filesystem::path &csv_path)
{
	for (int i = 1; i < argc; ++i) {
		const std::string option(argv[i]);
		if (option == "--help" || option == "-h") {
			print_usage(std::cout);
			return false;
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
		else {
			std::cerr << "Unknown option: " << option << '\n';
			return false;
		}
	}
	if (model_path.empty() || manifest_path.empty()) {
		print_usage(std::cerr);
		return false;
	}
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
	if (!parse_arguments(argc, argv, model_path, manifest_path, csv_path_value))
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
	std::ifstream manifest_input(manifest_path);
	if (!manifest_input) {
		std::cerr << "Cannot open manifest: " << manifest_path << '\n';
		return 2;
	}
	std::vector<frame_spec> frames;
	if (!faithfulframe::offline::read_manifest(manifest_input, manifest_path.parent_path(), frames, error)) {
		std::cerr << error << '\n';
		return 2;
	}
	if (!csv_path_value.empty()) {
		if (same_file_or_path(csv_path_value, model_path) || same_file_or_path(csv_path_value, manifest_path)) {
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
		"detector_work_us,detection_index,x0,y0,x1,y1,score\n";
	if (!*csv) {
		std::cerr << "Failed while writing CSV header\n";
		return 2;
	}

	face_detector_dlib_hog detector;
	detector.set_model(model_path.string().c_str());
	if (!detector.start()) {
		std::cerr << "Could not start the dlib detector worker\n";
		return 2;
	}
	std::vector<uint64_t> work_durations;
	uint64_t total_detections = 0;
	const auto replay_started = steady_clock::now();
	for (size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
		rgb_frame image;
		uint64_t decode_us = 0;
		if (!load_frame(frames[frame_index], image, decode_us, error)) {
			std::cerr << error << '\n';
			return 2;
		}
		if (image.width < 80 || image.height < 80) {
			std::cerr << "dlib HOG requires frames of at least 80x80 pixels: " << frames[frame_index].path
				  << '\n';
			return 2;
		}
		auto texture = make_texture(image, frames[frame_index].monotonic_ms);
		if (!texture) {
			std::cerr << "Could not prepare frame " << frames[frame_index].path << '\n';
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
		work_durations.push_back(detector_work_us);
		total_detections += detections.size();
		if (detections.empty()) {
			*csv << frame_index << ',' << frames[frame_index].monotonic_ms << ','
			     << csv_path(frames[frame_index].path) << ',' << image.width << ',' << image.height << ','
			     << decode_us << ',' << queue_latency_us << ',' << detector_work_us << ",,,,,,\n";
		} else {
			for (size_t detection_index = 0; detection_index < detections.size(); ++detection_index) {
				const rect_s &rect = detections[detection_index];
				*csv << frame_index << ',' << frames[frame_index].monotonic_ms << ','
				     << csv_path(frames[frame_index].path) << ',' << image.width << ',' << image.height
				     << ',' << decode_us << ',' << queue_latency_us << ',' << detector_work_us << ','
				     << detection_index << ',' << rect.x0 << ',' << rect.y0 << ',' << rect.x1 << ','
				     << rect.y1 << ',' << std::setprecision(6) << rect.score << '\n';
			}
		}
		if (!*csv) {
			std::cerr << "Failed while writing detector output\n";
			return 2;
		}
	}
	detector.stop();
	std::sort(work_durations.begin(), work_durations.end());
	const uint64_t total_work_us = std::accumulate(work_durations.begin(), work_durations.end(), uint64_t{0});
	const auto percentile = [&work_durations](double fraction) {
		const size_t index = static_cast<size_t>(fraction * static_cast<double>(work_durations.size() - 1));
		return work_durations[index];
	};
	const uint64_t replay_wall_us = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(steady_clock::now() - replay_started).count());
	std::cerr << "frames=" << frames.size() << " detections=" << total_detections
		  << " replay_wall_us=" << replay_wall_us
		  << " detector_mean_us=" << (total_work_us / work_durations.size())
		  << " detector_p50_us=" << percentile(0.50) << " detector_p95_us=" << percentile(0.95)
		  << " detector_max_us=" << work_durations.back() << '\n';
	csv->flush();
	if (!*csv) {
		std::cerr << "Failed while flushing detector output\n";
		return 2;
	}
	return 0;
}
