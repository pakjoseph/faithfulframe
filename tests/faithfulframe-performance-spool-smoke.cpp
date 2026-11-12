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

	csv_spool spool;
	CHECK(spool.open());
	CHECK(spool.append(first));
	CHECK(spool.row_count() == 1);
	CHECK(!spool.append(first)); // Metadata is permitted on the first row only.
	CHECK(spool.row_count() == 1);
	CHECK(spool.append(second));
	CHECK(spool.row_count() == 2);
	CHECK(!spool.export_to(output_directory.filePath("missing/run.csv")));

	const QString exported_path = output_directory.filePath("run.csv");
	CHECK(spool.export_to(exported_path));
	QFile exported_file(exported_path);
	CHECK(exported_file.open(QIODevice::ReadOnly));
	const QByteArray exported = exported_file.readAll();
	const std::string expected = csv_header() + csv_row(first) + csv_row(second);
	CHECK(exported == QByteArray(expected.data(), static_cast<int>(expected.size())));
	return 0;
}
