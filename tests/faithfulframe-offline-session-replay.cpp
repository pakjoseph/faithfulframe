#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "faithfulframe-offline-frame-sequence.hpp"
#include "face-tracker-association.hpp"
#include "face-detector-dlib-hog.h"
#include "face-tracker-manager.hpp"
#include "faithfulframe-candidate-limits.hpp"
#include "faithfulframe_ffi.h"
#include "texture-object.h"

#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/serialize.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// face_tracker_manager's property/default helpers are linked from the shared
// production object but are never called by this headless replay executable.
extern "C" obs_module_t *obs_current_module(void)
{
	return nullptr;
}

extern "C" const char *obs_module_text(const char *lookup_string)
{
	return lookup_string;
}

namespace {

using faithfulframe::offline::frame_spec;
using faithfulframe::offline::rgb_frame;
using steady_clock = std::chrono::steady_clock;

struct arguments
{
	std::filesystem::path model;
	std::filesystem::path manifest;
	std::filesystem::path trace;
	bool ppm_stdin = false;
	uint32_t fps = 30;
	uint32_t output_width = 1920;
	uint32_t output_height = 1080;
	bool has_operator_selection_time = false;
	uint64_t operator_selection_time_ms = 0;
	size_t operator_candidate_index = 0;
};

class offline_tracking_manager final : public face_tracker_manager {
public:
	std::shared_ptr<texture_object> current_frame;

protected:
	std::shared_ptr<texture_object> get_cvtex() override { return current_frame; }
};

class session_owner {
	ff_session *value = nullptr;

public:
	~session_owner()
	{
		if (value)
			ff_session_destroy(value);
	}
	ff_session **out() { return &value; }
	ff_session *get() const { return value; }
};

void usage(std::ostream &out)
{
	out << "Usage: faithfulframe-offline-session-replay --model <frontal_face_detector.dat> "
	       "(--manifest <frames.tsv> | --ppm-stdin [--fps <1..120>]) [--trace <trace.jsonl>] "
	       "[--output-width <pixels>] [--output-height <pixels>] "
	       "[--operator-select-first-candidate-at-ms <timestamp>]\n"
	    << "Manifest format: monotonic_ms<TAB>relative-or-absolute-frame-path. Frames must be P6 PPM, max value 255.\n"
	    << "The replay uses the production dlib HOG detector, face_tracker_manager, dlib correlation trackers, and Rust session policy.\n"
	    << "It records numeric candidate and crop traces only; it does not save video frames or face imagery. "
	       "Speaker selection is never automatic. The optional operator-selection flag simulates one explicit command.\n";
}

bool parse_integer(const std::string &text, uint64_t &value)
{
	if (text.empty())
		return false;
	const char *begin = text.data();
	const char *end = begin + text.size();
	const auto parsed = std::from_chars(begin, end, value, 10);
	return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_arguments(int argc, char **argv, arguments &args)
{
	for (int i = 1; i < argc; ++i) {
		const std::string option(argv[i]);
		if (option == "--help" || option == "-h") {
			usage(std::cout);
			return false;
		}
		if (option == "--ppm-stdin") {
			args.ppm_stdin = true;
			continue;
		}
		if (i + 1 >= argc) {
			std::cerr << "Missing value for " << option << '\n';
			return false;
		}
		const std::string value(argv[++i]);
		if (option == "--model") {
			args.model = value;
		} else if (option == "--manifest") {
			args.manifest = value;
		} else if (option == "--trace") {
			args.trace = value;
		} else if (option == "--fps") {
			uint64_t parsed = 0;
			if (!parse_integer(value, parsed) || parsed < 1 || parsed > 120) {
				std::cerr << "--fps must be an integer from 1 through 120\n";
				return false;
			}
			args.fps = static_cast<uint32_t>(parsed);
		} else if (option == "--output-width" || option == "--output-height") {
			uint64_t parsed = 0;
			if (!parse_integer(value, parsed) || parsed < 80 || parsed > 16384) {
				std::cerr << option << " must be an integer from 80 through 16384\n";
				return false;
			}
			if (option == "--output-width")
				args.output_width = static_cast<uint32_t>(parsed);
			else
				args.output_height = static_cast<uint32_t>(parsed);
		} else if (option == "--operator-select-first-candidate-at-ms") {
			if (!parse_integer(value, args.operator_selection_time_ms)) {
				std::cerr << option << " must be a non-negative integer timestamp\n";
				return false;
			}
			args.has_operator_selection_time = true;
		} else {
			std::cerr << "Unknown option: " << option << '\n';
			return false;
		}
	}
	if (args.model.empty() || (args.ppm_stdin == !args.manifest.empty())) {
		usage(std::cerr);
		return false;
	}
	return true;
}

bool same_file_or_path(const std::filesystem::path &left, const std::filesystem::path &right)
{
	if (left.lexically_normal() == right.lexically_normal())
		return true;
	std::error_code error;
	if (std::filesystem::equivalent(left, right, error) && !error)
		return true;
	error.clear();
	const auto left_canonical = std::filesystem::weakly_canonical(left, error);
	if (error)
		return false;
	const auto right_canonical = std::filesystem::weakly_canonical(right, error);
	return !error && left_canonical == right_canonical;
}

bool validate_model(const std::filesystem::path &path)
{
	try {
		dlib::frontal_face_detector detector;
		dlib::deserialize(path.string()) >> detector;
		return true;
	} catch (const std::exception &error) {
		std::cerr << "Cannot deserialize dlib HOG model: " << error.what() << '\n';
	} catch (...) {
		std::cerr << "Cannot deserialize dlib HOG model: unknown error\n";
	}
	return false;
}

std::shared_ptr<texture_object> make_texture(rgb_frame &image, uint64_t timestamp_ms, uint64_t epoch)
{
	if (image.width > std::numeric_limits<uint32_t>::max() / 3)
		return nullptr;
	for (size_t i = 0; i < image.pixels.size(); i += 3)
		std::swap(image.pixels[i], image.pixels[i + 2]);

	obs_source_frame source_frame{};
	source_frame.data[0] = image.pixels.data();
	source_frame.linesize[0] = image.width * 3;
	source_frame.width = image.width;
	source_frame.height = image.height;
	source_frame.format = VIDEO_FORMAT_BGR3;
	auto texture = std::make_shared<texture_object>();
	texture->scale = 1.0f;
	texture->captured_at_ms = timestamp_ms;
	texture->detection_epoch = epoch;
	texture->set_texture_obsframe(&source_frame, 1);
	return texture;
}

bool wait_for_manager_workers(offline_tracking_manager &manager)
{
	const auto deadline = steady_clock::now() + std::chrono::minutes(2);
	while (steady_clock::now() < deadline) {
		bool idle = true;
		if (manager.detect) {
			if (manager.detect->has_failed())
				return false;
			idle = manager.detect->try_idle() && idle;
		}
		const auto check_trackers = [&idle](const auto &trackers) {
			for (const auto &tracker : trackers) {
				if (!tracker.tracker)
					continue;
				if (tracker.tracker->has_failed()) {
					idle = false;
					continue;
				}
				if (tracker.tracker->trylock() != 0) {
					idle = false;
					continue;
				}
				tracker.tracker->unlock();
			}
		};
		check_trackers(manager.trackers);
		check_trackers(manager.trackers_idlepool);
		if (idle)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

const char *state_name(uint32_t value)
{
	switch (value) {
	case FF_STATE_FROZEN:
		return "frozen";
	case FF_STATE_MANUAL:
		return "manual";
	case FF_STATE_SEARCHING:
		return "searching";
	case FF_STATE_LOCKED:
		return "locked";
	case FF_STATE_LOST:
		return "lost";
	default:
		return "invalid";
	}
}

const char *reason_name(uint32_t value)
{
	switch (value) {
	case FF_REASON_STARTUP:
		return "startup";
	case FF_REASON_OPERATOR_FREEZE:
		return "operator_freeze";
	case FF_REASON_MANUAL_OVERRIDE:
		return "manual_override";
	case FF_REASON_AWAITING_SELECTION:
		return "awaiting_selection";
	case FF_REASON_SELECTED_BY_OPERATOR:
		return "selected_by_operator";
	case FF_REASON_CONTINUITY_RECOVERED:
		return "continuity_recovered";
	case FF_REASON_TARGET_EVIDENCE_STALE:
		return "target_evidence_stale";
	case FF_REASON_TARGET_LOSS_DEADLINE_EXPIRED:
		return "target_loss_deadline_expired";
	case FF_REASON_RESET_WIDE:
		return "reset_wide";
	case FF_REASON_LIFECYCLE_INVALIDATED:
		return "lifecycle_invalidated";
	case FF_REASON_STALE_COMMAND:
		return "stale_command";
	case FF_REASON_INVALID_COMMAND:
		return "invalid_command";
	case FF_REASON_NON_MONOTONIC_EVENT:
		return "non_monotonic_event";
	case FF_REASON_INVALID_CANDIDATE:
		return "invalid_candidate";
	case FF_REASON_TRACKER_IDENTITY_EXPIRED:
		return "tracker_identity_expired";
	case FF_REASON_SESSION_CLOSING:
		return "session_closing";
	case FF_REASON_SESSION_CLOSED:
		return "session_closed";
	case FF_REASON_STALE_LIFECYCLE:
		return "stale_lifecycle";
	case FF_REASON_INVALID_LIFECYCLE:
		return "invalid_lifecycle";
	case FF_REASON_FRAMING_FAILURE:
		return "framing_failure";
	default:
		return "invalid";
	}
}

const char *crop_action_name(uint32_t value)
{
	switch (value) {
	case FF_CROP_HOLD:
		return "hold";
	case FF_CROP_WIDE:
		return "wide";
	case FF_CROP_TRACK:
		return "track";
	case FF_CROP_NONE:
		return "none";
	default:
		return "invalid";
	}
}

const char *command_result_name(uint32_t value)
{
	switch (value) {
	case FF_COMMAND_NOT_APPLICABLE:
		return "not_a_command";
	case FF_COMMAND_APPLIED:
		return "applied";
	case FF_COMMAND_REJECTED:
		return "rejected";
	default:
		return "invalid";
	}
}

void write_snapshot(std::ostream &out, const ff_snapshot &snapshot)
{
	out << "\"snapshot\":{\"state\":\"" << state_name(snapshot.state) << "\",\"reason\":\""
	    << reason_name(snapshot.reason) << "\",\"crop_action\":\"" << crop_action_name(snapshot.crop_action)
	    << "\",\"command_result\":\"" << command_result_name(snapshot.command_result)
	    << "\",\"rejection_reason\":" << snapshot.rejection_reason
	    << ",\"status_version\":" << snapshot.status_version
	    << ",\"observation_version\":" << snapshot.observation_version << ",\"selected_handle\":";
	if (snapshot.has_selected)
		out << snapshot.selected_handle;
	else
		out << "null";
	out << ",\"generations\":{" << "\"source\":" << snapshot.generations.source
	    << ",\"session\":" << snapshot.generations.session << ",\"stream\":" << snapshot.generations.stream
	    << ",\"geometry\":" << snapshot.generations.geometry << ",\"tracker\":" << snapshot.generations.tracker
	    << "},\"source_crop\":[" << snapshot.crop.x << ',' << snapshot.crop.y << ',' << snapshot.crop.width << ','
	    << snapshot.crop.height << "],\"output_viewport\":[" << snapshot.viewport.x << ',' << snapshot.viewport.y
	    << ',' << snapshot.viewport.width << ',' << snapshot.viewport.height << "]}";
}

void write_snapshot_event(std::ostream &out, const char *kind, uint64_t frame_index, uint64_t at_ms,
			  const ff_snapshot &snapshot)
{
	out << "{\"kind\":\"" << kind << "\",\"frame_index\":" << frame_index << ",\"at_ms\":" << at_ms << ',';
	write_snapshot(out, snapshot);
	out << "}\n";
}

bool next_observation_version(uint64_t &version)
{
	if (version >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		return false;
	++version;
	return version != 0;
}

bool next_stream_timestamp(uint64_t frame_index, uint32_t fps, uint64_t &timestamp_ms)
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

bool valid_candidate_rect(const face_tracker_manager::tracker_rect_s &tracker, uint32_t width, uint32_t height,
			  ff_rect &normalized)
{
	if (!tracker.candidate_handle || !tracker.continuity_token || !ff_tracker_score_is_usable(tracker.rect.score) ||
	    !width || !height)
		return false;
	const float x0 = std::clamp(static_cast<float>(tracker.rect.x0) / width, 0.0f, 1.0f);
	const float y0 = std::clamp(static_cast<float>(tracker.rect.y0) / height, 0.0f, 1.0f);
	const float x1 = std::clamp(static_cast<float>(tracker.rect.x1) / width, 0.0f, 1.0f);
	const float y1 = std::clamp(static_cast<float>(tracker.rect.y1) / height, 0.0f, 1.0f);
	if (!(x1 > x0 && y1 > y0) || !std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
	    !std::isfinite(y1))
		return false;
	normalized = ff_rect{x0, y0, x1 - x0, y1 - y0};
	return true;
}

bool apply_geometry_change(ff_session *session, ff_identity identity, ff_generations &generations, uint32_t width,
			   uint32_t height, uint64_t at_ms, uint64_t frame_index, std::ostream &trace,
			   ff_snapshot &snapshot)
{
	ff_generations next{};
	if (ff_lifecycle_next_generations(FF_LIFECYCLE_GEOMETRY_CHANGED, &generations, &next) != FF_OK)
		return false;
	ff_lifecycle event{FF_LIFECYCLE_GEOMETRY_CHANGED, identity, next, width, height};
	if (ff_session_lifecycle(session, at_ms, &event, &snapshot) != FF_OK ||
	    snapshot.generations.geometry != next.geometry)
		return false;
	generations = next;
	trace << "{\"kind\":\"lifecycle\",\"event\":\"geometry_changed\",\"frame_index\":" << frame_index
	      << ",\"at_ms\":" << at_ms << ",\"source_width\":" << width << ",\"source_height\":" << height << ',';
	write_snapshot(trace, snapshot);
	trace << "}\n";
	return static_cast<bool>(trace);
}

} // namespace

int main(int argc, char **argv)
{
	arguments args;
	if (!parse_arguments(argc, argv, args))
		return argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") ? 0 : 2;
	if (!std::filesystem::is_regular_file(args.model)) {
		std::cerr << "Model file does not exist: " << args.model << '\n';
		return 2;
	}
	if (!validate_model(args.model))
		return 2;

	std::vector<frame_spec> frames;
	std::string error;
	if (!args.ppm_stdin) {
		std::ifstream manifest(args.manifest);
		if (!manifest ||
		    !faithfulframe::offline::read_manifest(manifest, args.manifest.parent_path(), frames, error)) {
			std::cerr << (error.empty() ? "Cannot open manifest" : error) << '\n';
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

	if (!args.trace.empty()) {
		if (same_file_or_path(args.trace, std::filesystem::path(argv[0])) ||
		    same_file_or_path(args.trace, args.model) ||
		    (!args.ppm_stdin && same_file_or_path(args.trace, args.manifest))) {
			std::cerr << "Trace output must not replace the replay executable, model, or frame manifest\n";
			return 2;
		}
		for (const frame_spec &frame : frames) {
			if (same_file_or_path(args.trace, frame.path)) {
				std::cerr << "Trace output must not replace an input frame: " << frame.path << '\n';
				return 2;
			}
		}
	}
	std::ofstream trace_file;
	std::ostream *trace = &std::cout;
	if (!args.trace.empty()) {
		trace_file.open(args.trace, std::ios::out | std::ios::trunc);
		if (!trace_file) {
			std::cerr << "Cannot create trace output: " << args.trace << '\n';
			return 2;
		}
		trace = &trace_file;
	}
	trace->imbue(std::locale::classic());
	*trace << std::setprecision(9);

	const ff_identity identity{1, 1};
	ff_generations generations{1, 1, 1, 1, 1};
	session_owner session;
	std::unique_ptr<offline_tracking_manager> manager;
	uint32_t source_width = 0;
	uint32_t source_height = 0;
	uint64_t observation_version = 0;
	uint64_t frame_index = 0;
	uint64_t candidate_observations = 0;
	uint64_t selection_attempts = 0;
	bool candidate_observation_open = false;
	bool selection_attempted = false;
	bool selection_applied = false;
	uint64_t last_timestamp_ms = 0;
	bool have_timestamp = false;
	ff_snapshot snapshot{};

	const auto process_frame = [&](rgb_frame &image, uint64_t timestamp_ms) -> bool {
		if (image.width < 80 || image.height < 80) {
			error = "dlib HOG requires frames of at least 80x80 pixels";
			return false;
		}
		if (have_timestamp && timestamp_ms <= last_timestamp_ms) {
			error = "frame timestamps must be strictly increasing";
			return false;
		}
		const uint64_t delta_ms = have_timestamp ? timestamp_ms - last_timestamp_ms : 1000 / args.fps;
		const float delta_seconds = static_cast<float>(delta_ms) / 1000.0f;
		have_timestamp = true;
		last_timestamp_ms = timestamp_ms;

		const bool first_frame = !manager;
		const bool geometry_changed = !first_frame &&
					      (image.width != source_width || image.height != source_height);
		if (first_frame) {
			source_width = image.width;
			source_height = image.height;
			const ff_session_config config{5000,
						       500,
						       static_cast<uint32_t>(faithfulframe_max_candidates),
						       source_width,
						       source_height,
						       args.output_width,
						       args.output_height,
						       0.45f,
						       1.0f,
						       4.0f,
						       0.25f,
						       0.08f,
						       0.08f,
						       0.05f,
						       0.25f};
			if (ff_session_create(&config, &identity, &generations, session.out()) != FF_OK ||
			    !session.get()) {
				error = "Could not create the Rust tracking session";
				return false;
			}
			manager = std::make_unique<offline_tracking_manager>();
			manager->upsize_l = 0.2f;
			manager->upsize_r = 0.2f;
			manager->upsize_t = 0.3f;
			manager->upsize_b = 0.1f;
			manager->tracking_threshold = 1.0e-4f;
			manager->scale.store(1.0f, std::memory_order_relaxed);
			manager->detector_crop_l = 0;
			manager->detector_crop_r = 0;
			manager->detector_crop_t = 0;
			manager->detector_crop_b = 0;
			manager->detector_engine = face_tracker_manager::engine_dlib_hog;
			manager->detector_dlib_hog_model = args.model.string();
			auto *hog_detector = new face_detector_dlib_hog();
			hog_detector->set_model(args.model.string().c_str());
			manager->detect = hog_detector;
			if (!manager->detect->start()) {
				error = "Could not start the production dlib HOG detector worker";
				return false;
			}
			if (ff_session_snapshot(session.get(), &snapshot) != FF_OK) {
				error = "Could not read the initial Rust session snapshot";
				return false;
			}
		} else if (geometry_changed) {
			if (!apply_geometry_change(session.get(), identity, generations, image.width, image.height,
						   timestamp_ms, frame_index, *trace, snapshot)) {
				error = "Could not apply source geometry change to the Rust session";
				return false;
			}
			source_width = image.width;
			source_height = image.height;
			manager->reset_requested.store(true, std::memory_order_release);
		}

		auto frame =
			make_texture(image, timestamp_ms, manager->detection_epoch.load(std::memory_order_acquire));
		if (!frame) {
			error = "Could not prepare the video frame for dlib tracking";
			return false;
		}
		manager->current_frame = frame;
		manager->tick(delta_seconds);
		const uint64_t current_epoch = manager->detection_epoch.load(std::memory_order_acquire);
		manager->detector_input_epoch.store(current_epoch, std::memory_order_release);
		frame->detection_epoch = current_epoch;
		manager->post_render(true);
		if (!wait_for_manager_workers(*manager)) {
			error = "A production detector or tracker worker failed or timed out";
			return false;
		}

		if (ff_session_tick(session.get(), timestamp_ms, &snapshot) != FF_OK) {
			error = "Rust session rejected the replay clock event";
			return false;
		}
		write_snapshot_event(*trace, "tick", frame_index, timestamp_ms, snapshot);

		std::vector<ff_candidate> candidates;
		candidates.reserve(manager->tracker_rects.size());
		for (const auto &tracker : manager->tracker_rects) {
			if (candidates.size() == faithfulframe_max_candidates)
				break;
			ff_rect rect{};
			if (!valid_candidate_rect(tracker, source_width, source_height, rect) ||
			    tracker.observed_at_ms > timestamp_ms || timestamp_ms - tracker.observed_at_ms > 500)
				continue;
			if (candidates.empty() && !next_observation_version(observation_version)) {
				error = "Candidate observation version exhausted";
				return false;
			}
			const uint64_t expires_at = tracker.observed_at_ms > std::numeric_limits<uint64_t>::max() - 500
							    ? std::numeric_limits<uint64_t>::max()
							    : tracker.observed_at_ms + 500;
			candidates.push_back(ff_candidate{tracker.candidate_handle, tracker.continuity_token, identity,
							  observation_version, generations, tracker.observed_at_ms,
							  expires_at, rect});
		}
		std::array<size_t, 6> tracker_state_counts{};
		for (const auto &tracker : manager->trackers) {
			const size_t state = static_cast<size_t>(tracker.state);
			if (state < tracker_state_counts.size())
				++tracker_state_counts[state];
		}
		std::vector<rect_s> detections;
		manager->detect->lock();
		manager->detect->get_faces(detections);
		manager->detect->unlock();
		*trace << "{\"kind\":\"tracker_diagnostics\",\"frame_index\":" << frame_index
		       << ",\"at_ms\":" << timestamp_ms
		       << ",\"detector_work_count\":" << manager->detect->completed_work_count()
		       << ",\"detector_work_us\":" << manager->detect->last_work_duration_us()
		       << ",\"detection_count\":" << detections.size()
		       << ",\"active_tracker_count\":" << manager->trackers.size() << ",\"tracker_state_counts\":[";
		for (size_t i = 0; i < tracker_state_counts.size(); ++i) {
			if (i)
				*trace << ',';
			*trace << tracker_state_counts[i];
		}
		*trace << "],\"candidate_count\":" << candidates.size() << "}\n";
		if (!*trace) {
			error = "Could not write tracker diagnostics";
			return false;
		}

		if (!candidates.empty()) {
			for (ff_candidate &candidate : candidates) {
				if (ff_session_publish_candidate(session.get(), timestamp_ms, &candidate, &snapshot) !=
				    FF_OK) {
					error = "Rust session rejected a dlib tracker candidate";
					return false;
				}
				++candidate_observations;
				*trace << "{\"kind\":\"candidate\",\"frame_index\":" << frame_index
				       << ",\"at_ms\":" << timestamp_ms
				       << ",\"candidate\":{\"handle\":" << candidate.handle
				       << ",\"continuity_token\":" << candidate.continuity_token
				       << ",\"observation_version\":" << candidate.observation_version
				       << ",\"observed_at_ms\":" << candidate.observed_at_ms
				       << ",\"expires_at_ms\":" << candidate.expires_at_ms << ",\"rect\":["
				       << candidate.rect.x << ',' << candidate.rect.y << ',' << candidate.rect.width
				       << ',' << candidate.rect.height << "]},";
				write_snapshot(*trace, snapshot);
				*trace << "}\n";
				if (!*trace) {
					error = "Could not write the session trace";
					return false;
				}
			}
			candidate_observation_open = true;
		} else if (candidate_observation_open) {
			if (!next_observation_version(observation_version) ||
			    ff_session_clear_candidates(session.get(), timestamp_ms, &identity, &generations,
							observation_version, &snapshot) != FF_OK) {
				error = "Could not clear the expired candidate observation";
				return false;
			}
			candidate_observation_open = false;
			write_snapshot_event(*trace, "candidates_cleared", frame_index, timestamp_ms, snapshot);
		}

		if (!selection_attempted && args.has_operator_selection_time &&
		    timestamp_ms >= args.operator_selection_time_ms && !candidates.empty()) {
			selection_attempted = true;
			++selection_attempts;
			if (args.operator_candidate_index >= candidates.size()) {
				error = "The requested operator candidate index is not present in this frame";
				return false;
			}
			const ff_candidate &candidate = candidates[args.operator_candidate_index];
			const ff_command command{FF_COMMAND_SELECT_CANDIDATE,
						 identity,
						 snapshot.status_version,
						 generations,
						 candidate.handle,
						 candidate.observation_version};
			if (ff_session_apply_command(session.get(), timestamp_ms, &command, &snapshot) != FF_OK) {
				error = "Rust session rejected the explicit replay selection command";
				return false;
			}
			selection_applied = snapshot.command_result == FF_COMMAND_APPLIED;
			*trace << "{\"kind\":\"command\",\"command\":\"select_candidate\",\"origin\":\"explicit_operator_flag\","
			       << "\"frame_index\":" << frame_index << ",\"at_ms\":" << timestamp_ms
			       << ",\"candidate_handle\":" << candidate.handle
			       << ",\"observation_version\":" << candidate.observation_version << ',';
			write_snapshot(*trace, snapshot);
			*trace << "}\n";
			if (!*trace) {
				error = "Could not write the operator command trace";
				return false;
			}
		}

		++frame_index;
		return true;
	};

	if (args.ppm_stdin) {
		while (true) {
			rgb_frame image;
			const auto result = faithfulframe::offline::read_ppm_stream_frame(std::cin, image, error);
			if (result == faithfulframe::offline::ppm_stream_result::end_of_stream)
				break;
			if (result == faithfulframe::offline::ppm_stream_result::error) {
				std::cerr << error << '\n';
				return 2;
			}
			uint64_t timestamp_ms = 0;
			if (!next_stream_timestamp(frame_index, args.fps, timestamp_ms)) {
				std::cerr << "Frame count exceeded the supported timestamp range\n";
				return 2;
			}
			if (!process_frame(image, timestamp_ms)) {
				std::cerr << error << '\n';
				return 3;
			}
		}
	} else {
		for (const frame_spec &spec : frames) {
			rgb_frame image;
			std::ifstream input(spec.path, std::ios::binary);
			if (!input || !faithfulframe::offline::read_ppm(input, image, error)) {
				std::cerr << (error.empty() ? "Cannot open frame: " + spec.path.string()
							    : spec.path.string() + ": " + error)
					  << '\n';
				return 2;
			}
			if (!process_frame(image, spec.monotonic_ms)) {
				std::cerr << error << '\n';
				return 3;
			}
		}
	}

	if (frame_index == 0) {
		std::cerr << "Replay contains no frames\n";
		return 2;
	}
	if (!wait_for_manager_workers(*manager)) {
		std::cerr << "A production detector or tracker worker failed during replay shutdown\n";
		return 3;
	}
	manager.reset(); // Stop and join detector/tracker workers before releasing session policy.
	if (!selection_attempted && args.has_operator_selection_time) {
		std::cerr << "No candidate was available when the requested operator selection became eligible\n";
		return 3;
	}
	if (args.has_operator_selection_time && !selection_applied) {
		std::cerr << "The explicit operator selection was rejected by session policy\n";
		return 3;
	}
	if (ff_session_snapshot(session.get(), &snapshot) != FF_OK) {
		std::cerr << "Could not read the final Rust session snapshot\n";
		return 3;
	}
	*trace << "{\"kind\":\"summary\",\"frames\":" << frame_index
	       << ",\"candidate_observations\":" << candidate_observations
	       << ",\"operator_selection_attempts\":" << selection_attempts
	       << ",\"operator_selection_applied\":" << (selection_applied ? "true" : "false") << ',';
	write_snapshot(*trace, snapshot);
	*trace << "}\n";
	trace->flush();
	if (!*trace) {
		std::cerr << "Failed while flushing the session trace\n";
		return 2;
	}
	return 0;
}
