#include "faithfulframe-performance-log.hpp"

#include <cmath>
#include <iostream>
#include <string>

#define CHECK(condition)                                                                             \
	do {                                                                                         \
		if (!(condition)) {                                                                  \
			std::cerr << "check failed at line " << __LINE__ << ": " #condition << '\n'; \
			return __LINE__;                                                             \
		}                                                                                    \
	} while (false)

static size_t csv_field_count(const std::string &row)
{
	size_t fields = 1;
	bool quoted = false;
	for (size_t index = 0; index < row.size(); ++index) {
		if (row[index] == '"') {
			if (quoted && index + 1 < row.size() && row[index + 1] == '"') {
				++index;
			} else {
				quoted = !quoted;
			}
		} else if (row[index] == ',' && !quoted) {
			++fields;
		} else if (row[index] == '\n' && !quoted) {
			break;
		}
	}
	return fields;
}

int main()
{
	using faithfulframe::performance::csv_header;
	using faithfulframe::performance::csv_row;
	using faithfulframe::performance::max_samples;
	using faithfulframe::performance::sample;

	const std::string header = csv_header();
	CHECK(header.starts_with("schema_version,run_metadata_present,utc_timestamp,elapsed_milliseconds,"));
	CHECK(header.find("resident_bytes") != std::string::npos);
	CHECK(header.find("last_detector_microseconds") != std::string::npos);
	CHECK(header.find("hardware_and_workload_notes") != std::string::npos);
	CHECK(header.find("candidate") == std::string::npos);
	CHECK(header.find("image") == std::string::npos);
	CHECK(max_samples == 43200);
	CHECK(faithfulframe::performance::csv_field("=1+1") == "'=1+1");

	sample value;
	value.run_metadata_present = true;
	value.utc_timestamp = "2026-11-12T12:34:56.789Z";
	value.operating_system = "macOS 26.0";
	value.cpu_architecture = "arm64";
	value.obs_version = "33.0.1";
	value.plugin_version = "0.9.1";
	value.hardware_and_workload_notes = "Mac mini, 4K \"stage\" -> 1080p";
	value.elapsed_milliseconds = 1000;
	value.source_id = 7;
	value.session_id = 9;
	value.source_generation = 2;
	value.session_generation = 3;
	value.stream_generation = 4;
	value.geometry_generation = 5;
	value.tracker_generation = 6;
	value.status_version = 10;
	value.state = 1;
	value.reason = 2;
	value.source_width = 3840;
	value.source_height = 2160;
	value.output_width = 1920;
	value.output_height = 1080;
	value.status_snapshot_age_milliseconds = 12;
	value.process_cpu_percent = 22.5;
	value.resident_bytes = 4096;
	value.virtual_bytes = 8192;
	value.active_fps = 59.94;
	value.queue_depth = 1;
	value.max_detector_microseconds = 7000;
	const std::string row = csv_row(value);
	CHECK(row.starts_with(
		"1,1,2026-11-12T12:34:56.789Z,1000,7,9,2,macOS 26.0,arm64,33.0.1,0.9.1,\"Mac mini, 4K \"\"stage\"\" -> 1080p\",3,4,5,6,10,1,2,3840,2160,1920,1080,12,22.50,4096,8192,59.940,"));
	CHECK(row.ends_with(",7000\n"));
	CHECK(csv_field_count(header) == csv_field_count(row));

	value.utc_timestamp = "sample,with\"quote\r\nline";
	value.process_cpu_percent = std::nan("");
	const std::string escaped_row = csv_row(value);
	CHECK(escaped_row.find("\"sample,with\"\"quote\r\nline\"") != std::string::npos);
	CHECK(escaped_row.find(",12,,4096,8192,") != std::string::npos);
	CHECK(csv_field_count(header) == csv_field_count(escaped_row));
	return 0;
}
