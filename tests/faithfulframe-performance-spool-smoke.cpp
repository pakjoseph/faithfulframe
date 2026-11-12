#include "faithfulframe-performance-spool.hpp"

#include <QFile>
#include <QTemporaryDir>

#include <iostream>

#define CHECK(condition)                                                                             \
	do {                                                                                         \
		if (!(condition)) {                                                                  \
			std::cerr << "check failed at line " << __LINE__ << ": " #condition << '\n'; \
			return __LINE__;                                                             \
		}                                                                                    \
	} while (0)

int main()
{
	using faithfulframe::performance::csv_header;
	using faithfulframe::performance::csv_row;
	using faithfulframe::performance::csv_spool;
	using faithfulframe::performance::sample;

	QTemporaryDir output_directory;
	CHECK(output_directory.isValid());

	sample first;
	first.run_metadata_present = true;
	first.utc_timestamp = "2026-11-12T12:34:56.789Z";
	first.operating_system = "Test OS";
	first.cpu_architecture = "arm64";
	first.obs_version = "33.0.1";
	first.plugin_version = "0.9.1";
	first.hardware_and_workload_notes = "Test encoder and workload";
	first.elapsed_milliseconds = 1000;
	first.source_id = 1;
	first.session_id = 2;
	first.source_generation = 1;
	first.source_width = 3840;
	first.source_height = 2160;
	first.output_width = 1920;
	first.output_height = 1080;

	sample second = first;
	second.run_metadata_present = false;
	second.operating_system.clear();
	second.cpu_architecture.clear();
	second.obs_version.clear();
	second.plugin_version.clear();
	second.hardware_and_workload_notes.clear();
	second.elapsed_milliseconds = 2000;

	sample third = second;
	third.elapsed_milliseconds = 3000;

	csv_spool spool;
	CHECK(spool.open());
	CHECK(spool.append(first));
	CHECK(spool.row_count() == 1);
	CHECK(!spool.append(first)); // Metadata is permitted on the first row only.
	CHECK(spool.row_count() == 1);
	CHECK(spool.append(second));
	CHECK(spool.row_count() == 2);
	CHECK(!spool.export_to(output_directory.filePath("missing/run.csv")));
	CHECK(spool.append(third)); // A failed export must leave later appends at EOF.
	CHECK(spool.row_count() == 3);

	const QString exported_path = output_directory.filePath("run.csv");
	CHECK(spool.export_to(exported_path));
	QFile exported_file(exported_path);
	CHECK(exported_file.open(QIODevice::ReadOnly));
	const QByteArray exported = exported_file.readAll();
	const std::string expected = csv_header() + csv_row(first) + csv_row(second) + csv_row(third);
	CHECK(exported == QByteArray(expected.data(), static_cast<int>(expected.size())));

	csv_spool chunked_spool;
	CHECK(chunked_spool.open());
	CHECK(chunked_spool.append(first));
	std::string chunked_expected = csv_header() + csv_row(first);
	for (uint64_t index = 0; index < 1024; ++index) {
		sample repeated = second;
		repeated.elapsed_milliseconds = 3000 + static_cast<int64_t>(index);
		CHECK(chunked_spool.append(repeated));
		chunked_expected += csv_row(repeated);
	}
	CHECK(chunked_expected.size() > 64 * 1024);
	const QString chunked_path = output_directory.filePath("chunked.csv");
	CHECK(chunked_spool.export_to(chunked_path));
	CHECK(chunked_spool.append(third)); // A successful export also restores the append position.
	chunked_expected += csv_row(third);
	CHECK(chunked_spool.export_to(chunked_path));
	QFile chunked_file(chunked_path);
	CHECK(chunked_file.open(QIODevice::ReadOnly));
	const QByteArray chunked_exported = chunked_file.readAll();
	CHECK(chunked_exported == QByteArray(chunked_expected.data(), static_cast<int>(chunked_expected.size())));
	return 0;
}
