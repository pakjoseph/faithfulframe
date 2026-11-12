#pragma once

#include "faithfulframe-performance-log.hpp"

#include <QFileDevice>
#include <QSaveFile>
#include <QTemporaryFile>

#include <cstdint>
#include <string>

namespace faithfulframe::performance {

class csv_spool final {
public:
	csv_spool() { file_.setAutoRemove(true); }

	bool open()
	{
		if (opened_ || !file_.open())
			return false;
		opened_ = true;
		const std::string header = csv_header();
		if (!write_all(file_, header.data(), static_cast<qint64>(header.size()))) {
			valid_ = false;
			return false;
		}
		return true;
	}

	bool append(const sample &value)
	{
		if (!valid_ || !opened_ || !file_.isOpen() || rows_ >= max_samples ||
		    value.run_metadata_present != (rows_ == 0))
			return false;
		const std::string row = csv_row(value);
		const qint64 position = file_.pos();
		if (!write_all(file_, row.data(), static_cast<qint64>(row.size()))) {
			if (!file_.resize(position) || !file_.seek(position)) {
				valid_ = false;
			} else {
				file_.unsetError();
			}
			return false;
		}
		++rows_;
		return true;
	}

	bool export_to(const QString &path)
	{
		if (!valid_ || !opened_ || !rows_ || !file_.flush() || !file_.seek(0))
			return false;
		auto restore_append_position = [this](bool result) {
			if (!file_.seek(file_.size()))
				valid_ = false;
			return result && valid_;
		};
		QSaveFile destination(path);
		if (!destination.open(QIODevice::WriteOnly))
			return restore_append_position(false);
		bool copied = true;
		while (!file_.atEnd()) {
			const QByteArray chunk = file_.read(64 * 1024);
			if (chunk.isEmpty()) {
				if (file_.error() == QFileDevice::NoError && file_.atEnd())
					break;
				copied = false;
				break;
			}
			if (destination.write(chunk) != chunk.size()) {
				copied = false;
				break;
			}
		}
		bool exported = false;
		if (copied)
			exported = destination.commit();
		else
			destination.cancelWriting();
		return restore_append_position(exported);
	}

	uint64_t row_count() const { return rows_; }

private:
	static bool write_all(QIODevice &device, const char *bytes, qint64 size)
	{
		qint64 written = 0;
		while (written < size) {
			const qint64 result = device.write(bytes + written, size - written);
			if (result <= 0)
				return false;
			written += result;
		}
		return true;
	}

	QTemporaryFile file_;
	uint64_t rows_ = 0;
	bool opened_ = false;
	bool valid_ = true;
};

} // namespace faithfulframe::performance
