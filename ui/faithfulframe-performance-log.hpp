#pragma once

#include <cstddef>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace faithfulframe::performance {

inline constexpr uint32_t schema_version = 1;
inline constexpr size_t max_samples = 12 * 60 * 60;

struct sample
{
	bool run_metadata_present = false;
	std::string utc_timestamp;
	std::string operating_system;
	std::string cpu_architecture;
	std::string obs_version;
	std::string plugin_version;
	std::string hardware_and_workload_notes;
	int64_t elapsed_milliseconds = 0;
	uint64_t source_id = 0;
	uint64_t session_id = 0;
	uint64_t source_generation = 0;
	uint64_t session_generation = 0;
	uint64_t stream_generation = 0;
	uint64_t geometry_generation = 0;
	uint64_t tracker_generation = 0;
	uint64_t status_version = 0;
	uint32_t state = 0;
	uint32_t reason = 0;
	uint32_t source_width = 0;
	uint32_t source_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	int64_t status_snapshot_age_milliseconds = -1;
	double process_cpu_percent = -1.0;
	int64_t resident_bytes = -1;
	int64_t virtual_bytes = -1;
	double active_fps = -1.0;
	int64_t render_frames = -1;
	int64_t render_lagged_frames = -1;
	int64_t encode_frames = -1;
	int64_t encode_skipped_frames = -1;
	int64_t stream_frames = -1;
	int64_t stream_dropped_frames = -1;
	uint64_t queue_depth = 0;
	uint64_t queue_high_water = 0;
	uint64_t queue_oldest_age_microseconds = 0;
	uint64_t accepted_work = 0;
	uint64_t rejected_work = 0;
	uint64_t coalesced_work = 0;
	uint64_t last_queue_wait_microseconds = 0;
	uint64_t max_queue_wait_microseconds = 0;
	uint64_t last_policy_microseconds = 0;
	uint64_t max_policy_microseconds = 0;
	uint64_t detector_work_count = 0;
	uint64_t last_detector_microseconds = 0;
	uint64_t max_detector_microseconds = 0;
};

inline std::string csv_field(const std::string &value)
{
	std::string safe_value = value;
	size_t first = 0;
	while (first < safe_value.size() && std::isspace(static_cast<unsigned char>(safe_value[first])))
		++first;
	if (first < safe_value.size() && std::string("=+-@").find(safe_value[first]) != std::string::npos)
		safe_value.insert(0, 1, '\'');
	if (safe_value.find_first_of(",\"\r\n") == std::string::npos)
		return safe_value;
	std::string escaped;
	escaped.reserve(safe_value.size() + 2);
	escaped.push_back('"');
	for (char character : safe_value) {
		if (character == '"')
			escaped.push_back('"');
		escaped.push_back(character);
	}
	escaped.push_back('"');
	return escaped;
}

inline std::string csv_header()
{
	// Run-level metadata is populated on the first data row only.
	return "schema_version,run_metadata_present,utc_timestamp,elapsed_milliseconds,source_id,session_id,source_generation,"
	       "operating_system,cpu_architecture,obs_version,plugin_version,hardware_and_workload_notes,"
	       "session_generation,stream_generation,geometry_generation,tracker_generation,status_version,state,reason,"
	       "source_width,source_height,output_width,output_height,status_snapshot_age_milliseconds,"
	       "process_cpu_percent,resident_bytes,virtual_bytes,active_fps,render_frames,render_lagged_frames,"
	       "encode_frames,encode_skipped_frames,stream_frames,stream_dropped_frames,queue_depth,queue_high_water,"
	       "queue_oldest_age_microseconds,accepted_work,rejected_work,coalesced_work,last_queue_wait_microseconds,"
	       "max_queue_wait_microseconds,last_policy_microseconds,max_policy_microseconds,detector_work_count,"
	       "last_detector_microseconds,max_detector_microseconds\n";
}

inline std::string csv_row(const sample &value)
{
	std::ostringstream row;
	row.imbue(std::locale::classic());
	row << schema_version << ',' << (value.run_metadata_present ? 1 : 0) << ',' << csv_field(value.utc_timestamp)
	    << ',' << value.elapsed_milliseconds << ',' << value.source_id << ',' << value.session_id << ','
	    << value.source_generation << ',' << csv_field(value.operating_system) << ','
	    << csv_field(value.cpu_architecture) << ',' << csv_field(value.obs_version) << ','
	    << csv_field(value.plugin_version) << ',' << csv_field(value.hardware_and_workload_notes) << ','
	    << value.session_generation << ',' << value.stream_generation << ',' << value.geometry_generation << ','
	    << value.tracker_generation << ',' << value.status_version << ',' << value.state << ',' << value.reason
	    << ',' << value.source_width << ',' << value.source_height << ',' << value.output_width << ','
	    << value.output_height << ',';
	if (value.status_snapshot_age_milliseconds >= 0)
		row << value.status_snapshot_age_milliseconds;
	row << ',';
	if (std::isfinite(value.process_cpu_percent) && value.process_cpu_percent >= 0.0)
		row << std::fixed << std::setprecision(2) << value.process_cpu_percent << std::defaultfloat;
	row << ',';
	if (value.resident_bytes >= 0)
		row << value.resident_bytes;
	row << ',';
	if (value.virtual_bytes >= 0)
		row << value.virtual_bytes;
	row << ',';
	if (std::isfinite(value.active_fps) && value.active_fps >= 0.0)
		row << std::fixed << std::setprecision(3) << value.active_fps << std::defaultfloat;
	row << ',';
	if (value.render_frames >= 0)
		row << value.render_frames;
	row << ',';
	if (value.render_lagged_frames >= 0)
		row << value.render_lagged_frames;
	row << ',';
	if (value.encode_frames >= 0)
		row << value.encode_frames;
	row << ',';
	if (value.encode_skipped_frames >= 0)
		row << value.encode_skipped_frames;
	row << ',';
	if (value.stream_frames >= 0)
		row << value.stream_frames;
	row << ',';
	if (value.stream_dropped_frames >= 0)
		row << value.stream_dropped_frames;
	row << ',' << value.queue_depth << ',' << value.queue_high_water << ',' << value.queue_oldest_age_microseconds
	    << ',' << value.accepted_work << ',' << value.rejected_work << ',' << value.coalesced_work << ','
	    << value.last_queue_wait_microseconds << ',' << value.max_queue_wait_microseconds << ','
	    << value.last_policy_microseconds << ',' << value.max_policy_microseconds << ','
	    << value.detector_work_count << ',' << value.last_detector_microseconds << ','
	    << value.max_detector_microseconds << '\n';
	return row.str();
}

} // namespace faithfulframe::performance
