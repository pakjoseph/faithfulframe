#include <obs-module.h>
#include <obs-frontend-api.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <QAction>
#include <QMainWindow>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QComboBox>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QRectF>
#include <QTimer>
#include <QSignalBlocker>
#include <QPointer>
#include <QVariantMap>
#include <QDateTime>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSysInfo>
#include "sanctuary-style.hpp"
#include "plugin-macros.generated.h"
#include "face-tracker-dock.hpp"
#include "face-tracker-widget.hpp"
#include "face-tracker-dock-internal.hpp"
#include "faithfulframe-candidate-selector.hpp"
#include "faithfulframe-performance-spool.hpp"
#include "faithfulframe-source-identity.hpp"
#ifdef ENABLE_RUST_CORE_BRIDGE
#include "faithfulframe-candidate-selection.hpp"
#include "faithfulframe-adapter-lease.hpp"
#include "faithfulframe-preview-geometry.hpp"
#include "faithfulframe_ffi.h"
#endif

#define SAVE_DATA_NAME "face-tracker-dock"
#define OBJ_NAME_SUFFIX "_ft_dock"

static bool dock_frontend_shutdown_started = false;
static bool dock_frontend_closing_docks = false;

void FTDock::closeEvent(QCloseEvent *event)
{
	if (!dock_frontend_shutdown_started && !dock_frontend_closing_docks &&
	    (performanceRecording || performanceSampleCount != 0)) {
		QMessageBox prompt(this);
		prompt.setWindowTitle("FaithfulFrame performance data");
		prompt.setText("This dock has performance data that has not been exported.");
		prompt.setInformativeText(
			"The CSV contains aggregate system and tracking metrics plus your hardware/workload note as entered. Do not include stream keys, server URLs, camera/source or speaker names, or personal details. It does not contain preview images or face data.");
		QPushButton *save = prompt.addButton("Export CSV", QMessageBox::AcceptRole);
		QPushButton *discard = prompt.addButton("Discard data", QMessageBox::DestructiveRole);
		prompt.addButton(QMessageBox::Cancel);
		prompt.setDefaultButton(save);
		prompt.exec();
		if (prompt.clickedButton() == save) {
			performanceRecording = false;
			if (!exportPerformanceRecording()) {
				event->ignore();
				return;
			}
		} else if (prompt.clickedButton() == discard) {
			performanceRecording = false;
			performanceSpool.reset();
			performanceSampleCount = 0;
			performanceRecordingSourceId = 0;
			performanceRecordingSessionId = 0;
		} else {
			event->ignore();
			return;
		}
	}
	QFrame::closeEvent(event);
}

void FTDock::changeEvent(QEvent *event)
{
	QFrame::changeEvent(event);
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
		applyDesignTokens();
}

// accessed only from UI thread
static std::vector<FTDock *> *docks;
static QPointer<QAction> new_dock_action;
static QPointer<QObject> dock_action_context;
static bool dock_save_callback_registered = false;
static bool dock_event_callback_registered = false;

static void save_load_ft_docks(obs_data_t *save_data, bool saving, void *private_data);
static void ft_docks_frontend_event_cb(enum obs_frontend_event event, void *private_data);

#ifdef ENABLE_RUST_CORE_BRIDGE
struct faithfulframe_safe_preview
{
	bool enabled = false;
	bool available = false;
	bool session_closed = false;
	bool target_active = false;
	bool signal_connected = false;
	faithfulframe::safe_adapter_availability_issue configuration_issue =
		faithfulframe::safe_adapter_availability_issue::none;
	uint32_t state = 0;
	uint32_t reason = 0;
	uint32_t command_result = 0;
	uint32_t rejection_reason = 0;
	uint32_t source_width = 0;
	uint32_t source_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	long long status_snapshot_age_ms = -1;
	long long loss_remaining_ms = -1;
	uint64_t executor_queue_depth = 0;
	uint64_t executor_queue_high_water = 0;
	uint64_t executor_oldest_queue_age_us = 0;
	uint64_t executor_accepted_work = 0;
	uint64_t executor_rejected_work = 0;
	uint64_t executor_coalesced_work = 0;
	uint64_t executor_last_queue_wait_us = 0;
	uint64_t executor_max_queue_wait_us = 0;
	uint64_t executor_last_processing_us = 0;
	uint64_t executor_max_processing_us = 0;
	uint64_t detector_last_work_us = 0;
	uint64_t detector_max_work_us = 0;
	uint64_t detector_work_count = 0;
	uint64_t render_thread_tracker_work_last_us = 0;
	uint64_t render_thread_tracker_work_max_us = 0;
	uint64_t stagesurface_map_count = 0;
	uint64_t stagesurface_map_failure_count = 0;
	uint64_t stagesurface_map_last_us = 0;
	uint64_t stagesurface_map_max_us = 0;
	uint64_t source_id = 0;
	uint64_t session_id = 0;
	uint64_t status_version = 0;
	uint64_t observation_version = 0;
	uint64_t source_generation = 0;
	uint64_t session_generation = 0;
	uint64_t stream_generation = 0;
	uint64_t geometry_generation = 0;
	uint64_t tracker_generation = 0;
	ff_rect crop{0.0f, 0.0f, 1.0f, 1.0f};
	ff_rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
	QString target_name;
	QString framing_preset = "Custom settings";
	QList<QVariantMap> candidates;
};

static bool read_calldata_int(calldata_t *cd, const char *name, uint64_t &value)
{
	long long raw = 0;
	if (!calldata_get_int(cd, name, &raw) || raw < 0)
		return false;
	value = static_cast<uint64_t>(raw);
	return true;
}

static bool read_calldata_float(calldata_t *cd, const char *name, float &value)
{
	double raw = 0.0;
	if (!calldata_get_float(cd, name, &raw) || !std::isfinite(raw))
		return false;
	value = static_cast<float>(raw);
	return std::isfinite(value);
}

static QString obs_performance_metrics()
{
	if (!obs_video_active())
		return "OBS video: inactive";

	const double active_fps = obs_get_active_fps();
	const uint32_t render_total = obs_get_total_frames();
	const uint32_t render_lagged = obs_get_lagged_frames();
	const video_t *video = obs_get_video();
	const uint32_t encode_total = video ? video_output_get_total_frames(video) : 0;
	const uint32_t encode_skipped = video ? video_output_get_skipped_frames(video) : 0;

	const QString fps_text = std::isfinite(active_fps) && active_fps > 0.0
					 ? QString("%1 FPS").arg(active_fps, 0, 'f', 1)
					 : QString("FPS unavailable");
	const QString render_text =
		render_total > 0
			? QString("%1 / %2 frames (%3%)")
				  .arg(static_cast<qulonglong>(render_lagged))
				  .arg(static_cast<qulonglong>(render_total))
				  .arg(100.0 * static_cast<double>(render_lagged) / static_cast<double>(render_total),
				       0, 'f', 2)
			: QString("no frames yet");
	const QString encode_text =
		encode_total > 0
			? QString("%1 / %2 frames (%3%)")
				  .arg(static_cast<qulonglong>(encode_skipped))
				  .arg(static_cast<qulonglong>(encode_total))
				  .arg(100.0 * static_cast<double>(encode_skipped) / static_cast<double>(encode_total),
				       0, 'f', 2)
			: QString("no frames yet");
	QString stream_text = "not streaming";
	if (obs_frontend_streaming_active()) {
		obs_output_t *stream = obs_frontend_get_streaming_output();
		if (!stream) {
			stream_text = "output unavailable";
		} else {
			const int stream_total = obs_output_get_total_frames(stream);
			const int stream_dropped = obs_output_get_frames_dropped(stream);
			obs_output_release(stream);
			stream_text = stream_total > 0 ? QString("%1 / %2 frames (%3%)")
								 .arg(stream_dropped)
								 .arg(stream_total)
								 .arg(100.0 * static_cast<double>(stream_dropped) /
									      static_cast<double>(stream_total),
								      0, 'f', 2)
						       : QString("no frames yet");
		}
	}

	return QString("OBS: %1 · render lag %2\nEncoding skips: %3\nNetwork output drops: %4")
		.arg(fps_text)
		.arg(render_text)
		.arg(encode_text)
		.arg(stream_text);
}

struct obs_performance_sample
{
	bool video_configuration_available = false;
	std::string graphics_module;
	int64_t fps_num = -1;
	int64_t fps_den = -1;
	int64_t base_width = -1;
	int64_t base_height = -1;
	int64_t canvas_output_width = -1;
	int64_t canvas_output_height = -1;
	int64_t output_format = -1;
	int64_t colorspace = -1;
	int64_t range = -1;
	int64_t scale_type = -1;
	int64_t gpu_conversion = -1;
	double active_fps = -1.0;
	int64_t render_frames = -1;
	int64_t render_lagged_frames = -1;
	int64_t encode_frames = -1;
	int64_t encode_skipped_frames = -1;
	int64_t stream_frames = -1;
	int64_t stream_dropped_frames = -1;
};

static obs_performance_sample read_obs_performance_sample()
{
	obs_performance_sample result;
	obs_video_info video_info{};
	if (obs_get_video_info(&video_info)) {
		result.video_configuration_available = true;
		if (video_info.graphics_module)
			result.graphics_module = video_info.graphics_module;
		result.fps_num = video_info.fps_num;
		result.fps_den = video_info.fps_den;
		result.base_width = video_info.base_width;
		result.base_height = video_info.base_height;
		result.canvas_output_width = video_info.output_width;
		result.canvas_output_height = video_info.output_height;
		result.output_format = video_info.output_format;
		result.colorspace = video_info.colorspace;
		result.range = video_info.range;
		result.scale_type = video_info.scale_type;
		result.gpu_conversion = video_info.gpu_conversion ? 1 : 0;
	}
	if (!obs_video_active())
		return result;

	const double active_fps = obs_get_active_fps();
	if (std::isfinite(active_fps) && active_fps >= 0.0)
		result.active_fps = active_fps;
	result.render_frames = static_cast<int64_t>(obs_get_total_frames());
	result.render_lagged_frames = static_cast<int64_t>(obs_get_lagged_frames());
	const video_t *video = obs_get_video();
	if (video) {
		result.encode_frames = static_cast<int64_t>(video_output_get_total_frames(video));
		result.encode_skipped_frames = static_cast<int64_t>(video_output_get_skipped_frames(video));
	}
	if (obs_frontend_streaming_active()) {
		obs_output_t *stream = obs_frontend_get_streaming_output();
		if (stream) {
			result.stream_frames = obs_output_get_total_frames(stream);
			result.stream_dropped_frames = obs_output_get_frames_dropped(stream);
			obs_output_release(stream);
		}
	}
	return result;
}

static QString safe_framing_preset(obs_source_t *source)
{
	if (!source)
		return "Custom settings";
	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings)
		return "Custom settings";
	const char *name = obs_data_get_string(settings, "preset_name");
	obs_data_t *presets = obs_data_get_obj(settings, "presets");
	obs_data_t *preset = presets && name && *name ? obs_data_get_obj(presets, name) : nullptr;
	static const char *const safe_fields[] = {"safe_tracking_dead_zone_percent", "safe_tracking_max_zoom",
						  "safe_tracking_pan_response", "safe_tracking_zoom_response",
						  "safe_tracking_loss_grace_seconds"};

	bool matches = preset != nullptr;
	for (const char *field : safe_fields) {
		if (!matches || !obs_data_has_user_value(preset, field) ||
		    obs_data_get_double(settings, field) != obs_data_get_double(preset, field)) {
			matches = false;
			break;
		}
	}
	const QString result = matches ? QString("Framing preset: %1").arg(QString::fromUtf8(name))
				       : QString("Framing preset: Custom settings");
	if (preset)
		obs_data_release(preset);
	if (presets)
		obs_data_release(presets);
	obs_data_release(settings);
	return result;
}

static bool read_safe_preview(obs_source_t *source, faithfulframe_safe_preview &preview)
{
	if (!source)
		return false;
	proc_handler_t *handler = obs_source_get_proc_handler(source);
	if (!handler)
		return false;
	calldata_t cd;
	calldata_init(&cd);
	const bool called = proc_handler_call(handler, "get_faithfulframe_preview", &cd);
	if (!called) {
		calldata_free(&cd);
		return false;
	}
	calldata_get_bool(&cd, "safe_enabled", &preview.enabled);
	calldata_get_bool(&cd, "available", &preview.available);
	calldata_get_bool(&cd, "session_closed", &preview.session_closed);
	calldata_get_bool(&cd, "target_active", &preview.target_active);
	calldata_get_bool(&cd, "signal_connected", &preview.signal_connected);
	uint64_t configuration_issue = 0;
	if (read_calldata_int(&cd, "configuration_issue", configuration_issue))
		preview.configuration_issue =
			static_cast<faithfulframe::safe_adapter_availability_issue>(configuration_issue);
	long long snapshot_age_ms = -1;
	if (calldata_get_int(&cd, "status_snapshot_age_ms", &snapshot_age_ms))
		preview.status_snapshot_age_ms = snapshot_age_ms;
	calldata_get_int(&cd, "loss_remaining_ms", &preview.loss_remaining_ms);
	read_calldata_int(&cd, "executor_queue_depth", preview.executor_queue_depth);
	read_calldata_int(&cd, "executor_queue_high_water", preview.executor_queue_high_water);
	read_calldata_int(&cd, "executor_oldest_queue_age_us", preview.executor_oldest_queue_age_us);
	read_calldata_int(&cd, "executor_accepted_work", preview.executor_accepted_work);
	read_calldata_int(&cd, "executor_rejected_work", preview.executor_rejected_work);
	read_calldata_int(&cd, "executor_coalesced_work", preview.executor_coalesced_work);
	read_calldata_int(&cd, "executor_last_queue_wait_us", preview.executor_last_queue_wait_us);
	read_calldata_int(&cd, "executor_max_queue_wait_us", preview.executor_max_queue_wait_us);
	read_calldata_int(&cd, "executor_last_processing_us", preview.executor_last_processing_us);
	read_calldata_int(&cd, "executor_max_processing_us", preview.executor_max_processing_us);
	read_calldata_int(&cd, "detector_last_work_us", preview.detector_last_work_us);
	read_calldata_int(&cd, "detector_max_work_us", preview.detector_max_work_us);
	read_calldata_int(&cd, "detector_work_count", preview.detector_work_count);
	read_calldata_int(&cd, "last_render_thread_tracker_work_us", preview.render_thread_tracker_work_last_us);
	read_calldata_int(&cd, "max_render_thread_tracker_work_us", preview.render_thread_tracker_work_max_us);
	read_calldata_int(&cd, "stagesurface_map_count", preview.stagesurface_map_count);
	read_calldata_int(&cd, "stagesurface_map_failure_count", preview.stagesurface_map_failure_count);
	read_calldata_int(&cd, "last_stagesurface_map_us", preview.stagesurface_map_last_us);
	read_calldata_int(&cd, "max_stagesurface_map_us", preview.stagesurface_map_max_us);
	uint64_t value = 0;
	if (read_calldata_int(&cd, "state", value))
		preview.state = static_cast<uint32_t>(value);
	if (read_calldata_int(&cd, "reason", value))
		preview.reason = static_cast<uint32_t>(value);
	if (read_calldata_int(&cd, "command_result", value))
		preview.command_result = static_cast<uint32_t>(value);
	if (read_calldata_int(&cd, "rejection_reason", value))
		preview.rejection_reason = static_cast<uint32_t>(value);
	if (read_calldata_int(&cd, "source_id", preview.source_id) &&
	    read_calldata_int(&cd, "session_id", preview.session_id) &&
	    read_calldata_int(&cd, "status_version", preview.status_version) &&
	    read_calldata_int(&cd, "observation_version", preview.observation_version) &&
	    read_calldata_int(&cd, "source_generation", preview.source_generation) &&
	    read_calldata_int(&cd, "session_generation", preview.session_generation) &&
	    read_calldata_int(&cd, "stream_generation", preview.stream_generation) &&
	    read_calldata_int(&cd, "geometry_generation", preview.geometry_generation) &&
	    read_calldata_int(&cd, "tracker_generation", preview.tracker_generation)) {
		// Version tokens fit the signed OBS calldata integer type by contract.
	}
	if (read_calldata_int(&cd, "source_width", value))
		preview.source_width = static_cast<uint32_t>(value);
	if (read_calldata_int(&cd, "source_height", value))
		preview.source_height = static_cast<uint32_t>(value);
	if (read_calldata_int(&cd, "output_width", value))
		preview.output_width = static_cast<uint32_t>(value);
	if (read_calldata_int(&cd, "output_height", value))
		preview.output_height = static_cast<uint32_t>(value);
	read_calldata_float(&cd, "crop_x", preview.crop.x);
	read_calldata_float(&cd, "crop_y", preview.crop.y);
	read_calldata_float(&cd, "crop_width", preview.crop.width);
	read_calldata_float(&cd, "crop_height", preview.crop.height);
	read_calldata_float(&cd, "viewport_x", preview.viewport.x);
	read_calldata_float(&cd, "viewport_y", preview.viewport.y);
	read_calldata_float(&cd, "viewport_width", preview.viewport.width);
	read_calldata_float(&cd, "viewport_height", preview.viewport.height);
	const char *target_name = nullptr;
	calldata_get_string(&cd, "target_name", &target_name);
	if (target_name)
		preview.target_name = QString::fromUtf8(target_name);
	preview.framing_preset = safe_framing_preset(source);
	uint64_t candidate_count = 0;
	if (preview.available && read_calldata_int(&cd, "candidate_count", candidate_count)) {
		uint64_t candidate_source_id = 0, candidate_session_id = 0, candidate_observation_version = 0;
		uint64_t candidate_source_generation = 0, candidate_session_generation = 0;
		uint64_t candidate_stream_generation = 0, candidate_geometry_generation = 0;
		uint64_t candidate_tracker_generation = 0;
		const bool have_candidate_versions =
			read_calldata_int(&cd, "candidate_source_id", candidate_source_id) &&
			read_calldata_int(&cd, "candidate_session_id", candidate_session_id) &&
			read_calldata_int(&cd, "candidate_observation_version", candidate_observation_version) &&
			read_calldata_int(&cd, "candidate_source_generation", candidate_source_generation) &&
			read_calldata_int(&cd, "candidate_session_generation", candidate_session_generation) &&
			read_calldata_int(&cd, "candidate_stream_generation", candidate_stream_generation) &&
			read_calldata_int(&cd, "candidate_geometry_generation", candidate_geometry_generation) &&
			read_calldata_int(&cd, "candidate_tracker_generation", candidate_tracker_generation);
		if (!have_candidate_versions) {
			calldata_free(&cd);
			return true;
		}
		for (uint64_t i = 0; i < std::min<uint64_t>(candidate_count, 8); ++i) {
			const QByteArray suffix = QByteArray::number(static_cast<qulonglong>(i));
			QVariantMap candidate;
			const QByteArray handle_key = "candidate_handle_" + suffix;
			const QByteArray expiry_key = "candidate_expires_" + suffix;
			const QByteArray x_key = "candidate_x_" + suffix;
			const QByteArray y_key = "candidate_y_" + suffix;
			const QByteArray w_key = "candidate_width_" + suffix;
			const QByteArray h_key = "candidate_height_" + suffix;
			uint64_t handle = 0, expires = 0;
			float x = 0, y = 0, width = 0, height = 0;
			if (!read_calldata_int(&cd, handle_key.constData(), handle) || !handle ||
			    !read_calldata_int(&cd, expiry_key.constData(), expires) ||
			    !read_calldata_float(&cd, x_key.constData(), x) ||
			    !read_calldata_float(&cd, y_key.constData(), y) ||
			    !read_calldata_float(&cd, w_key.constData(), width) ||
			    !read_calldata_float(&cd, h_key.constData(), height) || x < 0 || y < 0 || width <= 0 ||
			    height <= 0 || x + width > 1 || y + height > 1)
				continue;
			candidate.insert("handle", QVariant::fromValue<qulonglong>(handle));
			candidate.insert("expires", QVariant::fromValue<qulonglong>(expires));
			candidate.insert("x", x);
			candidate.insert("y", y);
			candidate.insert("width", width);
			candidate.insert("height", height);
			candidate.insert("source_id", QVariant::fromValue<qulonglong>(candidate_source_id));
			candidate.insert("session_id", QVariant::fromValue<qulonglong>(candidate_session_id));
			candidate.insert("status_version", QVariant::fromValue<qulonglong>(preview.status_version));
			candidate.insert("observation_version",
					 QVariant::fromValue<qulonglong>(candidate_observation_version));
			candidate.insert("source_generation",
					 QVariant::fromValue<qulonglong>(candidate_source_generation));
			candidate.insert("session_generation",
					 QVariant::fromValue<qulonglong>(candidate_session_generation));
			candidate.insert("stream_generation",
					 QVariant::fromValue<qulonglong>(candidate_stream_generation));
			candidate.insert("geometry_generation",
					 QVariant::fromValue<qulonglong>(candidate_geometry_generation));
			candidate.insert("tracker_generation",
					 QVariant::fromValue<qulonglong>(candidate_tracker_generation));
			preview.candidates.push_back(candidate);
		}
	}
	calldata_free(&cd);
	return true;
}

static bool candidate_output_rect(const faithfulframe_safe_preview &preview, const QVariantMap &candidate,
				  QRectF &output)
{
	ff_preview_rect_d mapped{};
	const ff_rect rect{candidate.value("x").toFloat(), candidate.value("y").toFloat(),
			   candidate.value("width").toFloat(), candidate.value("height").toFloat()};
	if (!ff_preview_candidate_rect(preview.crop, preview.viewport, rect, mapped))
		return false;
	output = QRectF(QPointF(mapped.x, mapped.y), QPointF(mapped.x + mapped.width, mapped.y + mapped.height));
	return true;
}

static bool candidate_selection_is_current(const QVariantMap &candidate, const faithfulframe_safe_preview &preview)
{
	const uint64_t handle = candidate.value("handle").toULongLong();
	if (!handle || preview.candidates.isEmpty() || preview.candidates.size() > 8)
		return false;
	std::array<uint64_t, 8> current_handles{};
	for (qsizetype i = 0; i < preview.candidates.size(); ++i)
		current_handles[static_cast<size_t>(i)] = preview.candidates.at(i).value("handle").toULongLong();
	const ff_identity previous_identity{candidate.value("source_id").toULongLong(),
					    candidate.value("session_id").toULongLong()};
	const ff_generations previous_generations{candidate.value("source_generation").toULongLong(),
						  candidate.value("session_generation").toULongLong(),
						  candidate.value("stream_generation").toULongLong(),
						  candidate.value("geometry_generation").toULongLong(),
						  candidate.value("tracker_generation").toULongLong()};
	const ff_identity current_identity{preview.source_id, preview.session_id};
	const ff_generations current_generations{preview.source_generation, preview.session_generation,
						 preview.stream_generation, preview.geometry_generation,
						 preview.tracker_generation};
	size_t current_index = 0;
	return faithfulframe_candidate_restore_index(handle, previous_identity, previous_generations, current_identity,
						     current_generations, current_handles.data(),
						     preview.candidates.size(), current_index);
}

static void use_full_source_preview_transform(faithfulframe_safe_preview &preview)
{
	ff_preview_set_full_source_transform(preview.crop, preview.viewport, preview.output_width,
					     preview.output_height, preview.source_width, preview.source_height);
}

static bool enqueue_safe_command(obs_source_t *source, const faithfulframe_safe_preview &preview, uint32_t kind,
				 const QVariantMap *candidate = nullptr, QString *failure = nullptr)
{
	if (failure)
		*failure = "Command was unavailable; wait for status to refresh and try again";
	if (!source || !preview.enabled || !preview.available)
		return false;
	if (candidate) {
		// Selection commands carry the exact identity and versions of the preview
		// snapshot used to resolve or hit-test this opaque candidate handle.
		if (candidate->value("source_id").toULongLong() != preview.source_id ||
		    candidate->value("session_id").toULongLong() != preview.session_id ||
		    candidate->value("status_version").toULongLong() != preview.status_version ||
		    candidate->value("observation_version").toULongLong() != preview.observation_version ||
		    candidate->value("source_generation").toULongLong() != preview.source_generation ||
		    candidate->value("session_generation").toULongLong() != preview.session_generation ||
		    candidate->value("stream_generation").toULongLong() != preview.stream_generation ||
		    candidate->value("geometry_generation").toULongLong() != preview.geometry_generation ||
		    candidate->value("tracker_generation").toULongLong() != preview.tracker_generation) {
			if (failure)
				*failure =
					"Selection expired or changed. Choose the face again from the current preview.";
			return false;
		}
	}

	proc_handler_t *handler = obs_source_get_proc_handler(source);
	if (!handler) {
		if (failure)
			*failure = "Tracking controls are unavailable; refresh the preview and try again";
		return false;
	}
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_int(&cd, "kind", kind);
	calldata_set_int(&cd, "source_id", static_cast<long long>(preview.source_id));
	calldata_set_int(&cd, "session_id", static_cast<long long>(preview.session_id));
	calldata_set_int(&cd, "status_version", static_cast<long long>(preview.status_version));
	calldata_set_int(&cd, "source_generation", static_cast<long long>(preview.source_generation));
	calldata_set_int(&cd, "session_generation", static_cast<long long>(preview.session_generation));
	calldata_set_int(&cd, "stream_generation", static_cast<long long>(preview.stream_generation));
	calldata_set_int(&cd, "geometry_generation", static_cast<long long>(preview.geometry_generation));
	calldata_set_int(&cd, "tracker_generation", static_cast<long long>(preview.tracker_generation));
	calldata_set_int(&cd, "candidate_handle",
			 candidate ? static_cast<long long>(candidate->value("handle").toULongLong()) : 0);
	calldata_set_int(&cd, "observation_version", static_cast<long long>(preview.observation_version));
	const bool called = proc_handler_call(handler, "faithfulframe_command", &cd);
	bool queued = false;
	bool queue_full = false;
	calldata_get_bool(&cd, "queued", &queued);
	calldata_get_bool(&cd, "queue_full", &queue_full);
	calldata_free(&cd);
	if (called && queued)
		return true;
	if (failure) {
		if (queue_full)
			*failure = "Tracking is catching up. Wait a moment, then try the control again.";
		else
			*failure = "Command was stale or unavailable; wait for status to refresh and try again";
	}
	return false;
}

static bool send_safe_command(obs_source_t *source, uint32_t kind, const QVariantMap *candidate = nullptr,
			      QString *failure = nullptr)
{
	faithfulframe_safe_preview preview;
	if (!read_safe_preview(source, preview)) {
		if (failure)
			*failure = "Tracking status is unavailable; wait for it to refresh and try again";
		return false;
	}
	return enqueue_safe_command(source, preview, kind, candidate, failure);
}

static QString safe_state_text(const faithfulframe_safe_preview &preview)
{
	switch (preview.configuration_issue) {
	case faithfulframe::safe_adapter_availability_issue::another_adapter_owns_tracking:
		return "Safe tracking is enabled on another camera adapter. Disable it there, then toggle it off and on here";
	case faithfulframe::safe_adapter_availability_issue::no_source_selected:
		return "Choose a camera in the filter's Safe tracking settings, then wait for its video signal";
	case faithfulframe::safe_adapter_availability_issue::invalid_source_binding:
		return "The selected source is unavailable or has another face-tracker filter. Choose a live video input, remove any other tracker filter, then toggle safe tracking off and on to bind it again";
	case faithfulframe::safe_adapter_availability_issue::waiting_for_video_signal:
		return "Waiting for video from the selected camera; check that the camera is connected and producing a signal";
	case faithfulframe::safe_adapter_availability_issue::session_start_failed:
		return "Tracking could not start. Check the camera format, then toggle safe tracking off and on to retry";
	case faithfulframe::safe_adapter_availability_issue::session_worker_failed_safe_wide:
		return "The tracking worker stopped; the camera is using wide framing. Toggle safe tracking off and on to start a new session";
	case faithfulframe::safe_adapter_availability_issue::none:
		break;
	}
	if (preview.session_closed)
		return "Tracking session stopped; reselect the camera in Source Settings to resume";
	if (!preview.available)
		return "FaithfulFrame safe tracking: waiting for a valid source session";
	if (preview.command_result == FF_COMMAND_REJECTED) {
		if (!preview.signal_connected)
			return "Command rejected; the camera signal disconnected independently. Restore the camera, then select the speaker again.";
		if (!preview.target_active)
			return "Command rejected; the camera source or scene became inactive independently. Activate its scene, then select the speaker again.";
		if (preview.reason == FF_REASON_TARGET_EVIDENCE_STALE && preview.loss_remaining_ms >= 0)
			return QString("Command rejected; target loss began independently. Holding framing; same-speaker "
				       "reacquisition: %1 s, then select again.")
				.arg(preview.loss_remaining_ms / 1000.0, 0, 'f', 1);
		if (preview.reason == FF_REASON_TARGET_LOSS_DEADLINE_EXPIRED)
			return "Command rejected; the recovery timer expired independently. Select a face to start following again or reset wide.";
		if (preview.reason == FF_REASON_FRAMING_FAILURE)
			return "Command rejected; framing failed independently and wide view is active. Check camera/output geometry, then select a speaker again.";
		if (preview.reason == FF_REASON_LIFECYCLE_INVALIDATED)
			return "Command rejected; camera or tracking settings changed independently. Select a speaker again before following.";
		if (preview.reason == FF_REASON_TRACKER_IDENTITY_EXPIRED)
			return "Command rejected; tracker identity expired independently. Select a speaker again before following.";
		if (preview.rejection_reason == FF_REJECTION_STALE_COMMAND)
			return "Selection or command expired; choose again from the current preview";
		if (preview.rejection_reason == FF_REJECTION_SESSION_CLOSED)
			return "The tracking session has closed; reselect the camera in source settings";
		if (preview.rejection_reason == FF_REJECTION_INVALID_COMMAND && preview.state == FF_STATE_LOCKED)
			return "Use Manual Override before selecting a different speaker";
		if (preview.rejection_reason == FF_REJECTION_INVALID_COMMAND && preview.state == FF_STATE_FROZEN)
			return "Resume before selecting a speaker";
		return "Command was rejected without changing framing; review status and try a valid action";
	}
	if (!preview.signal_connected)
		return "Camera disconnected; restore the camera, then select the speaker again";
	if (!preview.target_active)
		return "Camera inactive; activate its scene, then select the speaker again";
	if (preview.reason == FF_REASON_FRAMING_FAILURE)
		return "Framing failed; wide view is active. Check camera/output geometry, then select the speaker again";
	if (preview.reason == FF_REASON_LIFECYCLE_INVALIDATED)
		return "Camera or tracking settings changed; select the speaker again before following";
	if (preview.reason == FF_REASON_TRACKER_IDENTITY_EXPIRED)
		return "Tracker identity changed; select the speaker again before following";
	switch (preview.state) {
	case FF_STATE_FROZEN:
		if (preview.reason == FF_REASON_RESET_WIDE)
			return "Frozen at wide framing; press Resume when ready";
		return "Frozen; press Resume when ready";
	case FF_STATE_MANUAL:
		if (preview.reason == FF_REASON_RESET_WIDE)
			return "Wide framing; select a face to follow again";
		if (preview.reason == FF_REASON_MANUAL_OVERRIDE)
			return "Manual override active; following stopped and current framing held. Select a face or reset wide.";
		return "Manual; select a face before following";
	case FF_STATE_SEARCHING:
		if (preview.reason == FF_REASON_AWAITING_SELECTION) {
			if (preview.loss_remaining_ms >= 0)
				return QString("No speaker selected; choose a visible face within %1 s to start following.")
					.arg(preview.loss_remaining_ms / 1000.0, 0, 'f', 1);
			return "No speaker selected; choose a visible face to start following";
		}
		if (preview.loss_remaining_ms >= 0)
			return QString("Target briefly lost; holding framing. Same-speaker reacquisition: %1 s; then select again.")
				.arg(preview.loss_remaining_ms / 1000.0, 0, 'f', 1);
		return "Searching; holding the last framing";
	case FF_STATE_LOCKED:
		return "Following the selected speaker; use Manual Override to change targets";
	case FF_STATE_LOST:
		return "Recovery window expired; select a face to start following again or reset wide";
	default:
		return "FaithfulFrame safe tracking is unavailable";
	}
}

static double safe_preview_zoom(const faithfulframe_safe_preview &preview)
{
	double zoom = 1.0;
	if (!ff_preview_zoom_factor(preview.crop, preview.source_width, preview.source_height, preview.output_width,
				    preview.output_height, zoom))
		return 1.0;
	return zoom;
}
#endif

static std::string generate_unique_name()
{
	for (int n = 0;; n++) {
		char name[32] = "FTDock";
		if (n)
			snprintf(name, sizeof(name), "FTDock-%d", n);
		bool found = false;
		if (docks)
			for (size_t i = 0; i < docks->size(); i++) {
				if ((*docks)[i]->name == name)
					found = true;
			}
		if (!found)
			return name;
	}
}

void ft_dock_add(const char *name, obs_data_t *props, bool show)
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = new FTDock(main_window);
	dock->name = name ? name : generate_unique_name();
	dock->setObjectName(QString::fromUtf8(dock->name.c_str()) + OBJ_NAME_SUFFIX);
	dock->setWindowTitle(dock->name.c_str());

	dock->load_properties(props);

	std::string id = "ft." + dock->name;
	if (!obs_frontend_add_dock_by_id(id.c_str(), dock->name.c_str(), static_cast<QWidget *>(dock))) {
		delete dock;
		return;
	}

	if (docks)
		docks->push_back(dock);

	if (show) {
		/* Workaround to show the dock */
		QMetaObject::invokeMethod(
			dock,
			[dock]() {
				auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
				if (!main_window)
					return;
				QList<QDockWidget *> dd = main_window->findChildren<QDockWidget *>();
				for (QDockWidget *d : dd) {
					if (d->widget() != dock)
						continue;
					d->setVisible(true);
				}
			},
			Qt::QueuedConnection);
	}
}

struct init_target_selector_s
{
	QComboBox *q;
	int index;
	const char *source_uuid;
	const char *filter_uuid;
	int match_index;
};

struct filter_uuid_search
{
	const char *uuid;
	obs_source_t *match;
};

static void find_filter_by_uuid(obs_source_t *, obs_source_t *filter, void *param)
{
	auto *lookup = static_cast<filter_uuid_search *>(param);
	if (!lookup->match && faithfulframe::source_identity_matches(lookup->uuid, obs_source_get_uuid(filter)))
		lookup->match = obs_source_get_ref(filter);
}

static obs_source_t *resolve_filter_by_uuid(obs_source_t *source, const char *uuid)
{
	if (!source || !uuid || !*uuid)
		return nullptr;
	filter_uuid_search lookup{uuid, nullptr};
	obs_source_enum_filters(source, find_filter_by_uuid, &lookup);
	return lookup.match;
}

static bool init_target_selector_matches_identity(struct init_target_selector_s *ctx, obs_source_t *source,
						  obs_source_t *filter)
{
	if (!faithfulframe::source_identity_matches(ctx->source_uuid, obs_source_get_uuid(source)))
		return false;

	const char *actual_filter_uuid = filter ? obs_source_get_uuid(filter) : nullptr;
	if (ctx->filter_uuid && *ctx->filter_uuid)
		return faithfulframe::source_identity_matches(ctx->filter_uuid, actual_filter_uuid);
	return !actual_filter_uuid || !*actual_filter_uuid;
}

static void init_target_selector_cb_add(struct init_target_selector_s *ctx, obs_source_t *source, obs_source_t *filter)
{
	QString text;
	QList<QVariant> val;

	const char *name = obs_source_get_name(source);
	const char *filter_name = NULL;
	const char *source_uuid = obs_source_get_uuid(source);
	const char *filter_uuid = NULL;
	text = QString::fromUtf8(name);
	val.append(QByteArray(name));
	val.append(QByteArray());

	if (filter) {
		filter_name = obs_source_get_name(filter);
		filter_uuid = obs_source_get_uuid(filter);
		text += " / ";
		text += QString::fromUtf8(filter_name);
	}
	val[1] = QByteArray(filter_name ? filter_name : "");
	val.append(QByteArray(source_uuid ? source_uuid : ""));
	val.append(QByteArray(filter_uuid ? filter_uuid : ""));

	if (ctx->index < ctx->q->count()) {
		ctx->q->setItemText(ctx->index, text);
		ctx->q->setItemData(ctx->index, val);
	} else
		ctx->q->insertItem(ctx->index, text, val);

	if (ctx->source_uuid && *ctx->source_uuid && init_target_selector_matches_identity(ctx, source, filter))
		ctx->match_index = ctx->index;

	ctx->index++;
}

static void init_target_selector_cb_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	auto *ctx = (struct init_target_selector_s *)param;

	const char *id = obs_source_get_id(child);
	if (!strcmp(id, "face_tracker_filter") || !strcmp(id, "face_tracker_ptz")) {
		init_target_selector_cb_add(ctx, parent, child);
	}
}

static bool init_target_selector_cb_source(void *data, obs_source_t *source)
{
	auto *ctx = (struct init_target_selector_s *)data;

	const char *id = obs_source_get_id(source);
	if (!strcmp(id, "face_tracker_source")) {
		init_target_selector_cb_add(ctx, source, NULL);
		return true;
	}

	obs_source_enum_filters(source, init_target_selector_cb_filter, data);

	return true;
}

static void init_target_selector(QComboBox *q, const char *source_uuid = NULL, const char *filter_uuid = NULL)
{
	QByteArray preserved_source_uuid;
	QByteArray preserved_filter_uuid;
	if (!source_uuid) {
		const QList<QVariant> current_data = q->currentData().toList();
		if (current_data.count() >= 4) {
			preserved_source_uuid = current_data[2].toByteArray();
			preserved_filter_uuid = current_data[3].toByteArray();
			if (!preserved_source_uuid.isEmpty()) {
				source_uuid = preserved_source_uuid.constData();
				filter_uuid = preserved_filter_uuid.constData();
			}
		}
	}

	QSignalBlocker blocker(q);
	q->clear();
	q->addItem(obs_module_text("Select a tracking source"), QVariantList());
	init_target_selector_s ctx = {q, 1, source_uuid, filter_uuid, -1};
	obs_enum_scenes(init_target_selector_cb_source, &ctx);
	obs_enum_sources(init_target_selector_cb_source, &ctx);
	q->setCurrentIndex(ctx.match_index >= 0 ? ctx.match_index : 0);
}

void FTDock::checkTargetSelector()
{
	init_target_selector(targetSelector);
	updateState();
}

FTDock::FTDock(QWidget *parent) : QFrame(parent)
{
	data = face_tracker_dock_create();

	data->src_monitor = obs_source_create_private("face_tracker_monitor", "monitor", NULL);

	resize(256, 256);
	setMinimumSize(128, 128);
	setAttribute(Qt::WA_DeleteOnClose);

	mainLayout = new QVBoxLayout(this);

	targetSelector = new QComboBox(this);
	targetSelector->setAccessibleName("Camera source with FaithfulFrame tracking filter");
	init_target_selector(targetSelector);
	mainLayout->addWidget(targetSelector);
	connect(targetSelector, &QComboBox::currentTextChanged, this, &FTDock::targetSelectorChanged);

	pauseButton = new QPushButton(obs_module_text("Pause"), this);
	pauseButton->setCheckable(true);
	pauseButton->setAccessibleName("Freeze automatic tracking");
	mainLayout->addWidget(pauseButton);
	connect(pauseButton, &QPushButton::clicked, this, &FTDock::pauseButtonClicked);

	resetButton = new QPushButton(obs_module_text("Reset"), this);
	resetButton->setAccessibleName("Reset tracking framing");
	mainLayout->addWidget(resetButton);
	connect(resetButton, &QPushButton::clicked, this, &FTDock::resetButtonClicked);

	autoFollowButton = new QPushButton("Auto Follow", this);
	autoFollowButton->setAccessibleName("Enable automatic speaker following");
	mainLayout->addWidget(autoFollowButton);
	connect(autoFollowButton, &QPushButton::clicked, this, &FTDock::autoFollowButtonClicked);

	manualOverrideButton = new QPushButton("Manual Override", this);
	manualOverrideButton->setAccessibleName("Stop automatic following and hold the current framing");
	mainLayout->addWidget(manualOverrideButton);
	connect(manualOverrideButton, &QPushButton::clicked, this, &FTDock::manualOverrideButtonClicked);

	candidateSelector = new QComboBox(this);
	candidateSelector->setAccessibleName("Detected face candidates");
	mainLayout->addWidget(candidateSelector);
	selectCandidateButton = new QPushButton("Select Speaker", this);
	selectCandidateButton->setAccessibleName("Select the highlighted face as the speaker");
	mainLayout->addWidget(selectCandidateButton);
	connect(selectCandidateButton, &QPushButton::clicked, this, &FTDock::selectCandidateButtonClicked);

	safeStatusLabel = new QLabel(this);
	safeStatusLabel->setWordWrap(true);
	safeStatusLabel->setAccessibleName("FaithfulFrame tracking status");
	mainLayout->addWidget(safeStatusLabel);
	performanceToggle = new QToolButton(this);
	performanceToggle->setText("Performance diagnostics");
	performanceToggle->setAccessibleName("Show performance diagnostics");
	performanceToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	performanceToggle->setArrowType(Qt::RightArrow);
	performanceToggle->setCheckable(true);
	performanceToggle->setAutoRaise(true);
	mainLayout->addWidget(performanceToggle);
	performanceLabel = new QLabel(this);
	performanceLabel->setWordWrap(true);
	performanceLabel->setAccessibleName("FaithfulFrame performance diagnostics");
	performanceLabel->hide();
	mainLayout->addWidget(performanceLabel);
	performanceRunNotes = new QLineEdit(this);
	performanceRunNotes->setPlaceholderText(
		"Computer/GPU, encoder settings + workload (e.g., PC model; H.264 preset; 4K → 1080p).");
	performanceRunNotes->setAccessibleName("Hardware and workload notes for reproducible performance measurements");
	performanceRunNotes->setToolTip(
		"Describe the computer/GPU, non-secret OBS encoder settings, and workload. This note is exported as entered, so omit stream keys, server URLs, camera/source and speaker names, and personal details. OS, CPU architecture, OBS and FaithfulFrame versions, OBS canvas/video settings, and input/output dimensions are recorded automatically.");
	performanceRunNotes->setMaxLength(512);
	performanceRunNotes->hide();
	mainLayout->addWidget(performanceRunNotes);
	performanceRecordButton = new QPushButton("Record performance data", this);
	performanceRecordButton->setAccessibleName("Record aggregate performance data for a rehearsal");
	performanceRecordButton->setToolTip(
		"Records one aggregate sample per second. The CSV does not contain preview images or face data.");
	performanceRecordButton->hide();
	mainLayout->addWidget(performanceRecordButton);
	connect(performanceRecordButton, &QPushButton::clicked, this, &FTDock::performanceRecordButtonClicked);
	connect(performanceToggle, &QToolButton::toggled, this, [this](bool expanded) {
		performanceToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		performanceLabel->setVisible(expanded && performanceToggle->isVisible());
		if (expanded) {
			if (!processCpuUsageInfo)
				processCpuUsageInfo = os_cpu_usage_info_start();
			if (!processMetricsTimer.isValid())
				processMetricsTimer.start();
		} else if (!performanceRecording) {
			stopPerformanceMetrics();
		}
	});
#ifndef ENABLE_RUST_CORE_BRIDGE
	autoFollowButton->hide();
	manualOverrideButton->hide();
	candidateSelector->hide();
	selectCandidateButton->hide();
	safeStatusLabel->hide();
	performanceToggle->hide();
	performanceLabel->hide();
	performanceRunNotes->hide();
	performanceRecordButton->hide();
#endif

	enableButton = new QPushButton("", this);
	enableButton->setCheckable(true);
	mainLayout->addWidget(enableButton);
	connect(enableButton, &QPushButton::clicked, this, &FTDock::enableButtonClicked);

	propertyButton = new QPushButton(obs_module_text("Properties"), this);
	mainLayout->addWidget(propertyButton);
	connect(propertyButton, &QPushButton::clicked, this, &FTDock::propertyButtonClicked);

	ftWidget = new FTWidget(data, this);
	mainLayout->addWidget(ftWidget);

	connect(ftWidget, &FTWidget::removeDock, this, &FTDock::removeDock);
	connect(ftWidget, &FTWidget::previewClicked, this, &FTDock::handlePreviewClick);

	notrackButton = new QCheckBox(obs_module_text("Show all region"), this);
	mainLayout->addWidget(notrackButton);
	connect(notrackButton,
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
		&QCheckBox::stateChanged,
#else
		&QCheckBox::checkStateChanged,
#endif
		this, &FTDock::notrackButtonChanged);

	setLayout(mainLayout);
	design_tokens_ready = true;
	applyDesignTokens();

	connect(this, &FTDock::scenesMayChanged, this, &FTDock::checkTargetSelector);
	updateState();

	connect(this, &FTDock::dataChanged, this, &FTDock::updateWidget);
	statusTimer = new QTimer(this);
	statusTimer->setInterval(250);
	connect(statusTimer, &QTimer::timeout, this, &FTDock::updateState);
	statusTimer->start();
	sourceSelectorTimer = new QTimer(this);
	sourceSelectorTimer->setInterval(2000);
	connect(sourceSelectorTimer, &QTimer::timeout, this, &FTDock::checkTargetSelector);
	sourceSelectorTimer->start();
}

void FTDock::applyDesignTokens()
{
	if (!design_tokens_ready)
		return;
	mainLayout->setContentsMargins(sanctuary::style::space_4, sanctuary::style::space_4, sanctuary::style::space_4,
				       sanctuary::style::space_4);
	mainLayout->setSpacing(sanctuary::style::space_2);

	const QString button_style =
		sanctuary::style::control_stylesheet(sanctuary::style::colors_from_palette(palette()));
	for (QPushButton *button : {pauseButton, resetButton, enableButton, propertyButton, autoFollowButton,
				    manualOverrideButton, selectCandidateButton, performanceRecordButton}) {
		if (button)
			button->setStyleSheet(button_style);
	}
	for (QPushButton *button : {pauseButton, resetButton, autoFollowButton, manualOverrideButton,
				    selectCandidateButton, performanceRecordButton}) {
		if (button)
			button->setMinimumHeight(sanctuary::style::control_primary_height);
	}
	if (targetSelector)
		targetSelector->setMinimumHeight(sanctuary::style::control_standard_height);
	if (candidateSelector)
		candidateSelector->setMinimumHeight(sanctuary::style::control_standard_height);
}

FTDock::~FTDock()
{
	if (statusTimer)
		statusTimer->stop();
	if (sourceSelectorTimer)
		sourceSelectorTimer->stop();
	stopPerformanceMetrics();
	if (source_sh) {
		signal_handler_disconnect(source_sh, "state_changed", onStateChanged, this);
		source_sh = NULL;
	}
	obs_weak_source_release(selected_filter_ref);
	selected_filter_ref = nullptr;
	obs_weak_source_release(selected_source_ref);
	selected_source_ref = nullptr;

	face_tracker_dock_release(data);
	if (docks)
		for (size_t i = 0; i < docks->size(); i++) {
			if ((*docks)[i] == this) {
				docks->erase(docks->begin() + i);
				break;
			}
		}
}

void FTDock::showEvent(QShowEvent *)
{
	blog(LOG_INFO, "FTDock::showEvent");
}

void FTDock::hideEvent(QHideEvent *)
{
	blog(LOG_INFO, "FTDock::hideEvent");
}

// Keep frontend lifecycle handling module-scoped. Destroying docks from an
// event callback must not mutate OBS's callback list while it is being iterated.
static void ft_docks_frontend_event_cb(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		dock_frontend_shutdown_started = true;
		// OBS dispatches EXIT before invalidating the frontend callback API. Remove
		// the save hook here so module unload does not call through a dead API.
		if (dock_save_callback_registered) {
			obs_frontend_remove_save_callback(save_load_ft_docks, NULL);
			dock_save_callback_registered = false;
		}
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || event == OBS_FRONTEND_EVENT_EXIT) {
		dock_frontend_closing_docks = true;
		if (docks)
			while (docks->size()) {
				(*docks)[docks->size() - 1]->close();
				delete (*docks)[docks->size() - 1];
			}
		dock_frontend_closing_docks = false;
	}
}

void FTDock::targetSelectorChanged()
{
	if (updating_widget)
		return;

	bindSelectedTarget();
	operatorNotice.clear();
	operatorNoticeTimer.invalidate();
	updateState();
}

void FTDock::bindSelectedTarget()
{
	obs_weak_source_release(selected_filter_ref);
	selected_filter_ref = nullptr;
	obs_weak_source_release(selected_source_ref);
	selected_source_ref = nullptr;

	const QList<QVariant> data = targetSelector->currentData().toList();
	if (data.count() < 4)
		return;
	const QByteArray source_uuid = data[2].toByteArray();
	if (source_uuid.isEmpty())
		return;

	obs_source_t *source = obs_get_source_by_uuid(source_uuid.constData());
	if (!source)
		return;
	if (!faithfulframe::source_identity_matches(source_uuid.constData(), obs_source_get_uuid(source))) {
		obs_source_release(source);
		return;
	}
	selected_source_ref = obs_source_get_weak_source(source);

	const QByteArray filter_uuid = data[3].toByteArray();
	if (!filter_uuid.isEmpty()) {
		obs_source_t *filter = resolve_filter_by_uuid(source, filter_uuid.constData());
		if (filter) {
			selected_filter_ref = obs_source_get_weak_source(filter);
			obs_source_release(filter);
		}
	}
	obs_source_release(source);
}

OBSSource FTDock::get_source()
{
	const QList<QVariant> data = targetSelector->currentData().toList();
	if (data.count() < 4)
		return {};

	const QByteArray source_uuid = data[2].toByteArray();
	if (source_uuid.isEmpty() || !selected_source_ref)
		return {};

	obs_source_t *raw_source = obs_weak_source_get_source(selected_source_ref);
	if (!raw_source)
		return {};
	if (!faithfulframe::source_identity_matches(source_uuid.constData(), obs_source_get_uuid(raw_source))) {
		obs_source_release(raw_source);
		return {};
	}
	OBSSource source(raw_source);
	obs_source_release(raw_source);

	const QByteArray filter_uuid = data[3].toByteArray();
	if (filter_uuid.isEmpty())
		return source;
	if (!selected_filter_ref)
		return {};

	obs_source_t *raw_filter = obs_weak_source_get_source(selected_filter_ref);
	if (!raw_filter)
		return {};
	if (!faithfulframe::source_identity_matches(filter_uuid.constData(), obs_source_get_uuid(raw_filter))) {
		obs_source_release(raw_filter);
		return {};
	}
	obs_source_t *current_filter = resolve_filter_by_uuid(source, filter_uuid.constData());
	const bool filter_is_still_attached = current_filter == raw_filter;
	if (current_filter)
		obs_source_release(current_filter);
	if (!filter_is_still_attached) {
		obs_source_release(raw_filter);
		return {};
	}
	OBSSource target(raw_filter);
	obs_source_release(raw_filter);
	return target;
}

static bool is_filter(QComboBox *targetSelector)
{
	const QList<QVariant> data = targetSelector->currentData().toList();
	return data.count() >= 4 && !data[3].toByteArray().isEmpty();
}

static inline void set_monitor(obs_source_t *monitor, const QList<QVariant> &target_data)
{
	blog(LOG_INFO, "set_monitor monitor=%p", monitor);
	OBSData data = obs_data_create();
	obs_data_release(data);

	obs_data_set_string(data, "source_name",
			    target_data.count() >= 1 ? target_data[0].toByteArray().constData() : "");
	obs_data_set_string(data, "filter_name",
			    target_data.count() >= 2 ? target_data[1].toByteArray().constData() : "");
	obs_data_set_string(data, "source_uuid",
			    target_data.count() >= 4 ? target_data[2].toByteArray().constData() : "");
	obs_data_set_string(data, "filter_uuid",
			    target_data.count() >= 4 ? target_data[3].toByteArray().constData() : "");

	obs_source_update(monitor, data);
}

static void set_enable_button(QPushButton *enableButton, bool is_filter, bool is_enabled)
{
	enableButton->setEnabled(is_filter);
	enableButton->setChecked(is_filter && is_enabled);
	if (is_filter && is_enabled)
		enableButton->setText(obs_module_text("Disable filter"));
	else
		enableButton->setText(obs_module_text("Enable filter"));
}

static void set_pause_button(QPushButton *pauseButton, bool is_paused)
{
	pauseButton->setEnabled(true);
	pauseButton->setChecked(is_paused);
	if (is_paused) {
		pauseButton->setText(obs_module_text("Resume"));
		pauseButton->setAccessibleName("Resume automatic tracking");
	} else {
		pauseButton->setText(obs_module_text("Pause"));
		pauseButton->setAccessibleName("Freeze automatic tracking");
	}
}

void FTDock::onStateChanged(void *data, calldata_t *)
{
	FTDock *dock = (FTDock *)data;
	QMetaObject::invokeMethod(dock, "updateState", Qt::QueuedConnection);
}

void FTDock::updateState()
{
	OBSSource target = get_source();
	if (!target) {
		if (targetSelector->currentIndex() > 0) {
			QSignalBlocker blocker(targetSelector);
			targetSelector->setCurrentIndex(0);
		}
		bindSelectedTarget();
	}

	if (data && data->src_monitor) {
		pthread_mutex_lock(&data->mutex);
		set_monitor(data->src_monitor, targetSelector->currentData().toList());
		pthread_mutex_unlock(&data->mutex);
	}

	proc_handler_t *ph = target ? obs_source_get_proc_handler(target) : nullptr;
	if (!ph) {
		pauseButton->setEnabled(false);
		resetButton->setEnabled(false);
		enableButton->setEnabled(false);
		propertyButton->setEnabled(false);
		operatorNotice.clear();
		operatorNoticeTimer.invalidate();
		if (performanceRecording) {
			performanceRecording = false;
			operatorNotice =
				"Performance recording stopped because its tracking source is unavailable. Export the saved samples before starting another recording.";
			operatorNoticeTimer.start();
			if (!performanceToggle->isChecked())
				stopPerformanceMetrics();
		}
		if (performanceToggle->isChecked())
			performanceToggle->setChecked(false);
		autoFollowButton->hide();
		manualOverrideButton->hide();
		candidateSelector->hide();
		selectCandidateButton->hide();
		safeStatusLabel->hide();
		performanceToggle->hide();
		performanceLabel->hide();
		performanceRunNotes->hide();
		performanceRecordButton->setVisible(performanceSampleCount != 0);
		if (performanceSampleCount != 0) {
			performanceRecordButton->setText("Export CSV");
			performanceRecordButton->setAccessibleName("Export the saved aggregate performance data");
		}
		if (source_sh) {
			signal_handler_disconnect(source_sh, "state_changed", onStateChanged, this);
			source_sh = NULL;
		}
		return;
	}

	pauseButton->setEnabled(true);
	resetButton->setEnabled(true);
	propertyButton->setEnabled(true);

	in_updateState = true;
	bool safe_mode = false;
#ifdef ENABLE_RUST_CORE_BRIDGE
	faithfulframe_safe_preview safe_preview;
	read_safe_preview(target, safe_preview);
	safe_mode = safe_preview.enabled;
	autoFollowButton->setVisible(safe_mode);
	manualOverrideButton->setVisible(safe_mode);
	candidateSelector->setVisible(safe_mode);
	selectCandidateButton->setVisible(safe_mode);
	safeStatusLabel->setVisible(safe_mode);
	performanceToggle->setVisible(safe_mode);
	performanceLabel->setVisible(safe_mode && performanceToggle->isChecked());
	performanceRunNotes->setVisible(
		safe_mode && (performanceToggle->isChecked() || performanceRecording || performanceSampleCount != 0));
	performanceRunNotes->setEnabled(!performanceRecording);
	performanceRecordButton->setVisible(
		safe_mode && (performanceToggle->isChecked() || performanceRecording || performanceSampleCount != 0));
	enableButton->setVisible(!safe_mode);
	notrackButton->setVisible(true);
	if (safe_mode) {
		notrackButton->setText("Show full camera preview");
		notrackButton->setAccessibleName("Show full camera preview for selecting another speaker");
		notrackButton->setToolTip(
			"Changes this dock preview only. The tracking output and selected crop remain unchanged.");
		const QString tracking_source = safe_preview.target_name.isEmpty() ? "Unavailable"
										   : safe_preview.target_name;
		const QString resolution = safe_preview.source_width && safe_preview.source_height &&
							   safe_preview.output_width && safe_preview.output_height
						   ? QString("Input: %1 × %2  ·  Output: %3 × %4")
							     .arg(safe_preview.source_width)
							     .arg(safe_preview.source_height)
							     .arg(safe_preview.output_width)
							     .arg(safe_preview.output_height)
						   : QString("Resolution unavailable");
		QString status_text = QString("Tracking source: %1\n%2\n%3\nCurrent zoom: %4×\n%5")
					      .arg(tracking_source)
					      .arg(safe_preview.framing_preset)
					      .arg(safe_state_text(safe_preview))
					      .arg(safe_preview_zoom(safe_preview), 0, 'f', 1)
					      .arg(resolution);
		if (!operatorNotice.isEmpty()) {
			if (operatorNoticeTimer.isValid() && operatorNoticeTimer.elapsed() < 5000)
				status_text += "\n\n" + operatorNotice;
			else {
				operatorNotice.clear();
				operatorNoticeTimer.invalidate();
			}
		}
		safeStatusLabel->setText(status_text);
		if (performanceRecording && (safe_preview.source_id != performanceRecordingSourceId ||
					     safe_preview.session_id != performanceRecordingSessionId)) {
			performanceRecording = false;
			operatorNotice =
				"Performance recording stopped because the tracking source or session changed. Export the saved samples before starting another recording.";
			operatorNoticeTimer.start();
			if (!performanceToggle->isChecked())
				stopPerformanceMetrics();
		}
		performanceRecordButton->setText(
			performanceRecording
				? "Stop & Export CSV"
				: (performanceSampleCount == 0 ? "Record performance data" : "Export CSV"));
		performanceRecordButton->setAccessibleName(
			performanceRecording
				? "Stop recording and export aggregate performance data"
				: (performanceSampleCount == 0 ? "Record aggregate performance data for a rehearsal"
							       : "Export the saved aggregate performance data"));
		const bool diagnostics_active = performanceToggle->isChecked() || performanceRecording;
		if (diagnostics_active) {
			const bool sample_due = processMetricsTimer.isValid() && processMetricsTimer.elapsed() >= 1000;
			if (sample_due) {
				if (processCpuUsageInfo) {
					const double cpu_percent = os_cpu_usage_info_query(processCpuUsageInfo);
					processCpuPercent =
						std::isfinite(cpu_percent) && cpu_percent >= 0.0 ? cpu_percent : -1.0;
				}
				processMemoryAvailable = os_get_proc_memory_usage(&processMemoryUsage);
				processMetricsTimer.restart();
				if (performanceRecording &&
				    performanceSampleCount < faithfulframe::performance::max_samples) {
					faithfulframe::performance::sample sample;
					sample.utc_timestamp = QDateTime::currentDateTimeUtc()
								       .toString(Qt::ISODateWithMs)
								       .toStdString();
					// Keep run metadata on the first row only to bound the spool size.
					if (performanceSampleCount == 0) {
						sample.run_metadata_present = true;
						// Match OBS WebSocket GetVersion.platformDescription so a read-only
						// upstream baseline can be compared without OS-label translation.
						const QString operating_system = QSysInfo::prettyProductName();
						sample.operating_system = operating_system.toStdString();
						sample.cpu_architecture =
							QSysInfo::currentCpuArchitecture().toStdString();
						const char *obs_version = obs_get_version_string();
						sample.obs_version = obs_version ? obs_version : "unavailable";
						sample.plugin_version = PLUGIN_VERSION;
						sample.hardware_and_workload_notes =
							performanceHardwareAndWorkloadNotes.toStdString();
					}
					sample.elapsed_milliseconds = performanceRecordingTimer.elapsed();
					sample.source_id = safe_preview.source_id;
					sample.session_id = safe_preview.session_id;
					sample.source_generation = safe_preview.source_generation;
					sample.session_generation = safe_preview.session_generation;
					sample.stream_generation = safe_preview.stream_generation;
					sample.geometry_generation = safe_preview.geometry_generation;
					sample.tracker_generation = safe_preview.tracker_generation;
					sample.status_version = safe_preview.status_version;
					sample.state = safe_preview.state;
					sample.reason = safe_preview.reason;
					const obs_performance_sample obs_metrics = read_obs_performance_sample();
					sample.source_width = safe_preview.source_width;
					sample.source_height = safe_preview.source_height;
					sample.output_width = safe_preview.output_width;
					sample.output_height = safe_preview.output_height;
					sample.obs_video_configuration_available =
						obs_metrics.video_configuration_available;
					sample.obs_graphics_module = obs_metrics.graphics_module;
					sample.obs_fps_num = obs_metrics.fps_num;
					sample.obs_fps_den = obs_metrics.fps_den;
					sample.obs_base_width = obs_metrics.base_width;
					sample.obs_base_height = obs_metrics.base_height;
					sample.obs_canvas_output_width = obs_metrics.canvas_output_width;
					sample.obs_canvas_output_height = obs_metrics.canvas_output_height;
					sample.obs_output_format = obs_metrics.output_format;
					sample.obs_colorspace = obs_metrics.colorspace;
					sample.obs_range = obs_metrics.range;
					sample.obs_scale_type = obs_metrics.scale_type;
					sample.obs_gpu_conversion = obs_metrics.gpu_conversion;
					sample.status_snapshot_age_milliseconds = safe_preview.status_snapshot_age_ms;
					sample.process_cpu_percent = processCpuPercent;
					if (processMemoryAvailable) {
						sample.resident_bytes =
							static_cast<int64_t>(processMemoryUsage.resident_size);
						sample.virtual_bytes =
							static_cast<int64_t>(processMemoryUsage.virtual_size);
					}
					sample.active_fps = obs_metrics.active_fps;
					sample.render_frames = obs_metrics.render_frames;
					sample.render_lagged_frames = obs_metrics.render_lagged_frames;
					sample.encode_frames = obs_metrics.encode_frames;
					sample.encode_skipped_frames = obs_metrics.encode_skipped_frames;
					sample.stream_frames = obs_metrics.stream_frames;
					sample.stream_dropped_frames = obs_metrics.stream_dropped_frames;
					sample.queue_depth = safe_preview.executor_queue_depth;
					sample.queue_high_water = safe_preview.executor_queue_high_water;
					sample.queue_oldest_age_microseconds =
						safe_preview.executor_oldest_queue_age_us;
					sample.accepted_work = safe_preview.executor_accepted_work;
					sample.rejected_work = safe_preview.executor_rejected_work;
					sample.coalesced_work = safe_preview.executor_coalesced_work;
					sample.last_queue_wait_microseconds = safe_preview.executor_last_queue_wait_us;
					sample.max_queue_wait_microseconds = safe_preview.executor_max_queue_wait_us;
					sample.last_policy_microseconds = safe_preview.executor_last_processing_us;
					sample.max_policy_microseconds = safe_preview.executor_max_processing_us;
					sample.detector_work_count = safe_preview.detector_work_count;
					sample.last_detector_microseconds = safe_preview.detector_last_work_us;
					sample.max_detector_microseconds = safe_preview.detector_max_work_us;
					sample.last_render_thread_tracker_work_microseconds =
						safe_preview.render_thread_tracker_work_last_us;
					sample.max_render_thread_tracker_work_microseconds =
						safe_preview.render_thread_tracker_work_max_us;
					sample.stagesurface_map_count = safe_preview.stagesurface_map_count;
					sample.stagesurface_map_failure_count =
						safe_preview.stagesurface_map_failure_count;
					sample.last_stagesurface_map_microseconds =
						safe_preview.stagesurface_map_last_us;
					sample.max_stagesurface_map_microseconds = safe_preview.stagesurface_map_max_us;
					if (!performanceSpool || !performanceSpool->append(sample)) {
						performanceRecording = false;
						operatorNotice =
							"Performance recording stopped because its temporary CSV could not be written. ";
						if (performanceSampleCount == 0) {
							performanceSpool.reset();
							performanceRecordingSourceId = 0;
							performanceRecordingSessionId = 0;
						}
						operatorNotice +=
							performanceSampleCount
								? "Export the earlier samples or discard them."
								: "No samples were retained.";
						operatorNoticeTimer.start();
						if (!performanceToggle->isChecked())
							stopPerformanceMetrics();
					} else {
						++performanceSampleCount;
						if (performanceSampleCount == faithfulframe::performance::max_samples) {
							performanceRecording = false;
							operatorNotice =
								"Performance recording reached its 12-hour limit. Export the saved samples to continue.";
							operatorNoticeTimer.start();
							if (!performanceToggle->isChecked())
								stopPerformanceMetrics();
						}
					}
				}
			}
		}
		if (performanceToggle->isChecked()) {
			const QString process_cpu = processCpuPercent < 0.0
							    ? QString("sampling")
							    : QString("%1%").arg(processCpuPercent, 0, 'f', 1);
			const QString process_memory =
				processMemoryAvailable ? QString("%1 MiB resident / %2 MiB virtual")
								 .arg(processMemoryUsage.resident_size / (1024 * 1024))
								 .arg(processMemoryUsage.virtual_size / (1024 * 1024))
						       : QString("unavailable");
			const QString snapshot_age =
				safe_preview.status_snapshot_age_ms < 0
					? QString("unavailable")
					: QString("%1 ms").arg(safe_preview.status_snapshot_age_ms);
			QString diagnostics_text =
				QString("OBS process: CPU %1, memory %2\nSnapshot: %3 | queue: %4 / %5, oldest pending %6 µs | "
					"wait: %7 / %8 µs | policy: %9 / %10 µs | "
					"render-thread total: %11 / %12 µs | stage-map call (included): %13 / %14 µs "
					"(%15 maps, %16 failed) | dlib: %17 runs, %18 / %19 µs | "
					"accepted %20 · rejected %21 · coalesced %22\n%23")
					.arg(process_cpu)
					.arg(process_memory)
					.arg(snapshot_age)
					.arg(static_cast<qulonglong>(safe_preview.executor_queue_depth))
					.arg(static_cast<qulonglong>(safe_preview.executor_queue_high_water))
					.arg(static_cast<qulonglong>(safe_preview.executor_oldest_queue_age_us))
					.arg(static_cast<qulonglong>(safe_preview.executor_last_queue_wait_us))
					.arg(static_cast<qulonglong>(safe_preview.executor_max_queue_wait_us))
					.arg(static_cast<qulonglong>(safe_preview.executor_last_processing_us))
					.arg(static_cast<qulonglong>(safe_preview.executor_max_processing_us))
					.arg(static_cast<qulonglong>(safe_preview.render_thread_tracker_work_last_us))
					.arg(static_cast<qulonglong>(safe_preview.render_thread_tracker_work_max_us))
					.arg(static_cast<qulonglong>(safe_preview.stagesurface_map_last_us))
					.arg(static_cast<qulonglong>(safe_preview.stagesurface_map_max_us))
					.arg(static_cast<qulonglong>(safe_preview.stagesurface_map_count))
					.arg(static_cast<qulonglong>(safe_preview.stagesurface_map_failure_count))
					.arg(static_cast<qulonglong>(safe_preview.detector_work_count))
					.arg(static_cast<qulonglong>(safe_preview.detector_last_work_us))
					.arg(static_cast<qulonglong>(safe_preview.detector_max_work_us))
					.arg(static_cast<qulonglong>(safe_preview.executor_accepted_work))
					.arg(static_cast<qulonglong>(safe_preview.executor_rejected_work))
					.arg(static_cast<qulonglong>(safe_preview.executor_coalesced_work))
					.arg(obs_performance_metrics());
			if (performanceRecording)
				diagnostics_text += QString("\nRecording aggregate metrics · %1 samples")
							    .arg(static_cast<qulonglong>(performanceSampleCount));
			else if (performanceSampleCount != 0)
				diagnostics_text += QString("\n%1 aggregate samples ready to export")
							    .arg(static_cast<qulonglong>(performanceSampleCount));
			performanceLabel->setText(diagnostics_text);
		}
		set_pause_button(pauseButton, safe_preview.state == FF_STATE_FROZEN);
		pauseButton->setEnabled(safe_preview.available);
		resetButton->setText("Reset to Wide");
		resetButton->setAccessibleName("Reset framing to the full camera view");
		resetButton->setEnabled(safe_preview.available);
		autoFollowButton->setEnabled(safe_preview.available && (safe_preview.state == FF_STATE_MANUAL ||
									safe_preview.state == FF_STATE_LOST));
		manualOverrideButton->setEnabled(safe_preview.available && safe_preview.state != FF_STATE_FROZEN &&
						 safe_preview.state != FF_STATE_MANUAL);
		const QVariantMap previous_candidate = candidateSelector->currentData().toMap();
		const qulonglong previous_handle = previous_candidate.value("handle").toULongLong();
		faithfulframe_safe_preview display_preview = safe_preview;
		if (notrackButton->isChecked())
			use_full_source_preview_transform(display_preview);
		// Replacing the model while the popup is open resets its highlighted row,
		// disrupting keyboard operation. Keep the list stable until it closes, then
		// validate its selected opaque handle against the newest preview below.
		faithfulframe::ui::refresh_candidate_selector_model(
			candidateSelector->view() && candidateSelector->view()->isVisible(), [&] {
				QSignalBlocker blocker(candidateSelector);
				candidateSelector->clear();
				candidateSelector->addItem("Choose a detected face", QVariantMap{});
				std::array<uint64_t, 8> visible_candidate_handles{};
				int visible_candidate_count = 0;
				for (const QVariantMap &candidate : safe_preview.candidates) {
					QRectF visible;
					if (!candidate_output_rect(display_preview, candidate, visible))
						continue;
					const int x = qRound(visible.center().x() * 100.0);
					const int y = qRound(visible.center().y() * 100.0);
					candidateSelector->addItem(QString("Visible face %1 · preview %2%%, %3%%")
									   .arg(++visible_candidate_count)
									   .arg(x)
									   .arg(y),
								   candidate);
					visible_candidate_handles[static_cast<size_t>(visible_candidate_count - 1)] =
						candidate.value("handle").toULongLong();
				}
				if (!visible_candidate_count) {
					candidateSelector->setItemText(0, "No fresh face candidates");
				} else {
					size_t restore_index = 0;
					const ff_identity previous_identity{
						previous_candidate.value("source_id").toULongLong(),
						previous_candidate.value("session_id").toULongLong()};
					const ff_generations previous_generations{
						previous_candidate.value("source_generation").toULongLong(),
						previous_candidate.value("session_generation").toULongLong(),
						previous_candidate.value("stream_generation").toULongLong(),
						previous_candidate.value("geometry_generation").toULongLong(),
						previous_candidate.value("tracker_generation").toULongLong()};
					const ff_identity current_identity{safe_preview.source_id,
									   safe_preview.session_id};
					const ff_generations current_generations{safe_preview.source_generation,
										 safe_preview.session_generation,
										 safe_preview.stream_generation,
										 safe_preview.geometry_generation,
										 safe_preview.tracker_generation};
					if (faithfulframe_candidate_restore_index(
						    previous_handle, previous_identity, previous_generations,
						    current_identity, current_generations,
						    visible_candidate_handles.data(),
						    static_cast<size_t>(visible_candidate_count), restore_index))
						candidateSelector->setCurrentIndex(static_cast<int>(restore_index + 1));
				}
			});
		const bool has_explicit_candidate =
			candidate_selection_is_current(candidateSelector->currentData().toMap(), safe_preview);
		selectCandidateButton->setText("Select Speaker");
		selectCandidateButton->setAccessibleName("Select the highlighted face as the speaker");
		selectCandidateButton->setEnabled(safe_preview.available && has_explicit_candidate &&
						  (safe_preview.state == FF_STATE_MANUAL ||
						   safe_preview.state == FF_STATE_SEARCHING ||
						   safe_preview.state == FF_STATE_LOST));
	}
#else
	UNUSED_PARAMETER(safe_mode);
#endif
	if (!safe_mode) {
		operatorNotice.clear();
		operatorNoticeTimer.invalidate();
		if (performanceRecording) {
			performanceRecording = false;
			operatorNotice =
				"Performance recording stopped because safety tracking is no longer enabled. Export the saved samples before starting another recording.";
			operatorNoticeTimer.start();
			if (!performanceToggle->isChecked())
				stopPerformanceMetrics();
		}
		if (performanceToggle->isChecked())
			performanceToggle->setChecked(false);
		performanceRecordButton->setVisible(performanceSampleCount != 0);
		if (performanceSampleCount != 0) {
			performanceRecordButton->setText("Export CSV");
			performanceRecordButton->setAccessibleName("Export the saved aggregate performance data");
		}
		performanceRunNotes->hide();
		resetButton->setText(obs_module_text("Reset"));
		resetButton->setAccessibleName("Reset tracking framing");
		resetButton->setEnabled(true);
		notrackButton->setText(obs_module_text("Show all region"));
		notrackButton->setAccessibleName("Show all regions in the preview");
		notrackButton->setToolTip(QString());
	}

	calldata_t cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	if (!safe_mode && proc_handler_call(ph, "get_state", &cd)) {
		bool b;

		if (calldata_get_bool(&cd, "paused", &b)) {
			set_pause_button(pauseButton, b);
		}
	}

	set_enable_button(enableButton, !safe_mode && is_filter(targetSelector), obs_source_enabled(target));

	signal_handler_t *sh = obs_source_get_signal_handler(target);
	if (sh && sh != source_sh) {
		if (source_sh)
			signal_handler_disconnect(source_sh, "state_changed", onStateChanged, this);
		source_sh = sh;
		signal_handler_connect_ref(source_sh, "state_changed", onStateChanged, this);
	}

	in_updateState = false;
}

void FTDock::updateWidget()
{
	if (updating_widget)
		return;
	updating_widget = true;
	pthread_mutex_lock(&data->mutex);

	if (data->src_monitor) {
		obs_data_t *props = obs_source_get_settings(data->src_monitor);

		const char *source_uuid = obs_data_get_string(props, "source_uuid");
		const char *filter_uuid = obs_data_get_string(props, "filter_uuid");
		init_target_selector(targetSelector, source_uuid, filter_uuid);
		bindSelectedTarget();

		OBSSource selected = get_source();
		set_enable_button(enableButton, is_filter(targetSelector), selected && obs_source_enabled(selected));

		bool notrack = obs_data_get_bool(props, "notrack");
		notrackButton->setCheckState(notrack ? Qt::Checked : Qt::Unchecked);

		obs_data_release(props);
	}

	pthread_mutex_unlock(&data->mutex);
	updating_widget = false;

	updateState();
}

void FTDock::showOperatorNotice(const QString &message)
{
	operatorNotice = message;
	operatorNoticeTimer.start();
	updateState();
}

void FTDock::stopPerformanceMetrics()
{
	if (processCpuUsageInfo)
		os_cpu_usage_info_destroy(processCpuUsageInfo);
	processCpuUsageInfo = nullptr;
	processMetricsTimer.invalidate();
	processCpuPercent = -1.0;
	processMemoryUsage = {};
	processMemoryAvailable = false;
}

bool FTDock::exportPerformanceRecording()
{
	if (performanceSampleCount == 0)
		return true;
	if (!performanceSpool)
		return false;
	const QString suggested_name = QString("faithfulframe-performance-%1.csv")
					       .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss"));
	const QString path = QFileDialog::getSaveFileName(this, "Export FaithfulFrame performance data", suggested_name,
							  "CSV files (*.csv)");
	if (path.isEmpty())
		return false;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (!performanceSpool->export_to(path)) {
		showOperatorNotice("The CSV could not be saved. The samples are still available to export again.");
		return false;
	}
#else
	UNUSED_PARAMETER(path);
	return false;
#endif
	const uint64_t exported_samples = performanceSampleCount;
	performanceSpool.reset();
	performanceSampleCount = 0;
	performanceRecordingSourceId = 0;
	performanceRecordingSessionId = 0;
	if (!performanceToggle->isChecked())
		stopPerformanceMetrics();
	showOperatorNotice(
		QString("Exported %1 aggregate system, OBS video, and tracking samples with your run note. No preview images or face data were saved.")
			.arg(static_cast<qulonglong>(exported_samples)));
	return true;
}

void FTDock::performanceRecordButtonClicked()
{
	if (performanceRecording) {
		performanceRecording = false;
		performanceRecordButton->setText("Export CSV");
		if (!performanceToggle->isChecked())
			stopPerformanceMetrics();
		if (performanceSampleCount == 0) {
			showOperatorNotice(
				"No one-second sample has been captured yet. Start another recording and leave it running for at least one second.");
			performanceSpool.reset();
			return;
		}
		exportPerformanceRecording();
		return;
	}
	if (performanceSampleCount != 0) {
		exportPerformanceRecording();
		return;
	}

#ifdef ENABLE_RUST_CORE_BRIDGE
	OBSSource target = get_source();
	faithfulframe_safe_preview preview;
	if (!read_safe_preview(target, preview) || !preview.enabled || !preview.available || !preview.source_id ||
	    !preview.session_id) {
		showOperatorNotice("Performance recording needs an active safety-tracking source and session.");
		return;
	}
	performanceHardwareAndWorkloadNotes = performanceRunNotes->text().trimmed();
	if (performanceHardwareAndWorkloadNotes.isEmpty()) {
		showOperatorNotice(
			"Enter the computer/GPU, non-secret OBS encoder settings, and test workload before recording. Do not include stream keys or server URLs.");
		return;
	}
	performanceSpool = std::make_unique<faithfulframe::performance::csv_spool>();
	if (!performanceSpool->open()) {
		performanceSpool.reset();
		showOperatorNotice(
			"The temporary performance CSV could not be created. Check local disk availability.");
		return;
	}
	performanceRecordingSourceId = preview.source_id;
	performanceRecordingSessionId = preview.session_id;
	performanceSampleCount = 0;
	performanceRecordingTimer.start();
	performanceRecording = true;
	if (!processCpuUsageInfo)
		processCpuUsageInfo = os_cpu_usage_info_start();
	processMetricsTimer.start();
	performanceRecordButton->setText("Stop & Export CSV");
	showOperatorNotice(
		"Recording aggregate performance metrics once per second. Your note is exported as entered; omit stream keys, server URLs, camera/source and speaker names, and personal details. Preview images and face data are not collected.");
#else
	showOperatorNotice("Performance recording requires the FaithfulFrame safety tracking bridge.");
#endif
}

void FTDock::pauseButtonClicked(bool checked)
{
	if (in_updateState)
		return;

	OBSSource target = get_source();
#ifdef ENABLE_RUST_CORE_BRIDGE
	faithfulframe_safe_preview safe_preview;
	if (read_safe_preview(target, safe_preview) && safe_preview.enabled) {
		QString failure;
		if (!send_safe_command(target, checked ? FF_COMMAND_FREEZE : FF_COMMAND_RESUME, nullptr, &failure))
			showOperatorNotice(failure);
		return;
	}
#endif
	if (!target)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(target);
	if (!ph)
		return;

	blog(LOG_INFO, "FTDock::pauseButtonClicked checked=%d", (int)checked);

	calldata_t cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_bool(&cd, "paused", checked);
	proc_handler_call(ph, "set_state", &cd);

	set_pause_button(pauseButton, checked);
}

void FTDock::resetButtonClicked(bool checked)
{
	UNUSED_PARAMETER(checked);

	if (in_updateState)
		return;

	OBSSource target = get_source();
#ifdef ENABLE_RUST_CORE_BRIDGE
	faithfulframe_safe_preview safe_preview;
	if (read_safe_preview(target, safe_preview) && safe_preview.enabled) {
		QString failure;
		if (!send_safe_command(target, FF_COMMAND_RESET_TO_WIDE, nullptr, &failure))
			showOperatorNotice(failure);
		return;
	}
#endif
	if (!target)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(target);
	if (!ph)
		return;

	calldata_t cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_bool(&cd, "reset", true);
	proc_handler_call(ph, "set_state", &cd);
}

void FTDock::autoFollowButtonClicked()
{
#ifdef ENABLE_RUST_CORE_BRIDGE
	OBSSource target = get_source();
	QString failure;
	if (!send_safe_command(target, FF_COMMAND_ENABLE_AUTO_FOLLOW, nullptr, &failure))
		showOperatorNotice(failure);
#endif
}

void FTDock::manualOverrideButtonClicked()
{
#ifdef ENABLE_RUST_CORE_BRIDGE
	OBSSource target = get_source();
	QString failure;
	if (!send_safe_command(target, FF_COMMAND_MANUAL_OVERRIDE, nullptr, &failure))
		showOperatorNotice(failure);
#endif
}

void FTDock::selectCandidateButtonClicked()
{
#ifdef ENABLE_RUST_CORE_BRIDGE
	const QVariantMap selected_candidate = candidateSelector->currentData().toMap();
	const uint64_t selected_handle = selected_candidate.value("handle").toULongLong();
	if (selected_candidate.isEmpty() || !selected_handle) {
		showOperatorNotice("No fresh candidate is available. Wait for a face to appear in the preview.");
		return;
	}
	OBSSource target = get_source();
	faithfulframe_safe_preview preview;
	if (!read_safe_preview(target, preview) || !preview.enabled || !preview.available) {
		showOperatorNotice("The current preview is unavailable. Wait for tracking status to refresh.");
		return;
	}
	std::array<uint64_t, 8> current_handles{};
	for (qsizetype i = 0; i < preview.candidates.size(); ++i)
		current_handles[static_cast<size_t>(i)] = preview.candidates.at(i).value("handle").toULongLong();
	const ff_identity previous_identity{selected_candidate.value("source_id").toULongLong(),
					    selected_candidate.value("session_id").toULongLong()};
	const ff_generations previous_generations{selected_candidate.value("source_generation").toULongLong(),
						  selected_candidate.value("session_generation").toULongLong(),
						  selected_candidate.value("stream_generation").toULongLong(),
						  selected_candidate.value("geometry_generation").toULongLong(),
						  selected_candidate.value("tracker_generation").toULongLong()};
	const ff_identity current_identity{preview.source_id, preview.session_id};
	const ff_generations current_generations{preview.source_generation, preview.session_generation,
						 preview.stream_generation, preview.geometry_generation,
						 preview.tracker_generation};
	size_t current_index = 0;
	if (!faithfulframe_candidate_restore_index(selected_handle, previous_identity, previous_generations,
						   current_identity, current_generations, current_handles.data(),
						   preview.candidates.size(), current_index)) {
		showOperatorNotice("That face is no longer available. Choose a face from the current preview.");
		return;
	}
	// Keyboard selection is by stable opaque handle, not by the cached rectangle.
	// Resolve it against the current preview so routine detector updates do not
	// make an otherwise current selection command stale.
	const QVariantMap current_candidate = preview.candidates.at(static_cast<qsizetype>(current_index));
	QString failure;
	if (!enqueue_safe_command(target, preview, FF_COMMAND_SELECT_CANDIDATE, &current_candidate, &failure))
		showOperatorNotice(failure);
#endif
}

void FTDock::handlePreviewClick(const QPointF &position)
{
#ifdef ENABLE_RUST_CORE_BRIDGE
	OBSSource target = get_source();
	faithfulframe_safe_preview preview;
	if (!read_safe_preview(target, preview) || !preview.enabled || !preview.available || !preview.output_width ||
	    !preview.output_height || preview.candidates.isEmpty())
		return;
	if (preview.state == FF_STATE_LOCKED) {
		showOperatorNotice("Use Manual Override before selecting a different speaker.");
		return;
	}
	if (preview.state == FF_STATE_FROZEN) {
		showOperatorNotice("Resume before selecting a speaker.");
		return;
	}
	if (preview.state != FF_STATE_MANUAL && preview.state != FF_STATE_SEARCHING && preview.state != FF_STATE_LOST)
		return;
	if (notrackButton->isChecked())
		use_full_source_preview_transform(preview);

	// FTWidget and Qt mouse events use logical pixels. The monitor's rendered
	// preview uses the same aspect-fit transform, so normalizing inside the
	// letterboxed image accounts for both HiDPI scaling and padding.
	double x = 0.0, y = 0.0;
	if (!ff_preview_widget_point(position.x(), position.y(), ftWidget->width(), ftWidget->height(),
				     preview.output_width, preview.output_height, x, y))
		return;

	std::array<ff_preview_rect_d, 8> visible_rectangles{};
	std::array<const QVariantMap *, 8> visible_candidates{};
	std::size_t visible_count = 0;
	for (const QVariantMap &candidate : preview.candidates) {
		QRectF visible;
		if (!candidate_output_rect(preview, candidate, visible))
			continue;
		if (visible_count == visible_rectangles.size()) {
			showOperatorNotice("Candidate list changed. Refresh the preview before selecting a speaker.");
			return;
		}
		visible_rectangles[visible_count] =
			ff_preview_rect_d{visible.x(), visible.y(), visible.width(), visible.height()};
		visible_candidates[visible_count++] = &candidate;
	}
	std::size_t hit_index = 0;
	const ff_preview_hit_result hit_result =
		ff_preview_unique_hit(visible_rectangles.data(), visible_count, x, y, hit_index);
	if (hit_result != ff_preview_hit_result::unique_hit) {
		showOperatorNotice(hit_result == ff_preview_hit_result::ambiguous
					   ? "Overlapping faces are ambiguous; choose one from the candidate list"
					   : "Click inside one visible face to select its speaker");
		return;
	}
	const QVariantMap hit = *visible_candidates[hit_index];
	QString failure;
	if (!enqueue_safe_command(target, preview, FF_COMMAND_SELECT_CANDIDATE, &hit, &failure))
		showOperatorNotice(failure);
#else
	UNUSED_PARAMETER(position);
#endif
}

void FTDock::enableButtonClicked(bool checked)
{
	if (!is_filter(targetSelector))
		return;

	OBSSource target = get_source();

	if (target) {
		obs_source_set_enabled(target, checked);
		set_enable_button(enableButton, true, checked);
	}
}

void FTDock::propertyButtonClicked(bool checked)
{
	UNUSED_PARAMETER(checked);

	const QList<QVariant> data = targetSelector->currentData().toList();
	if (data.count() < 4)
		return;

	const QByteArray source_uuid = data[2].toByteArray();
	if (source_uuid.isEmpty() || !selected_source_ref)
		return;

	obs_source_t *target = obs_weak_source_get_source(selected_source_ref);
	if (!target) {
		return;
	}
	if (!faithfulframe::source_identity_matches(source_uuid.constData(), obs_source_get_uuid(target))) {
		obs_source_release(target);
		return;
	}

	if (data[3].toByteArray().isEmpty())
		obs_frontend_open_source_properties(target);
	else
		obs_frontend_open_source_filters(target);

	obs_source_release(target);
}

void FTDock::notrackButtonChanged(
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
	int state
#else
	Qt::CheckState state
#endif
)
{
	if (!data || !data->src_monitor)
		return;
	obs_data_t *props = obs_data_create();
	obs_data_set_bool(props, "notrack", state == Qt::Checked);
	obs_source_update(data->src_monitor, props);
	obs_data_release(props);
}

static void save_load_ft_docks(obs_data_t *save_data, bool saving, void *)
{
	blog(LOG_INFO, "save_load_ft_docks saving=%d", (int)saving);
	if (!docks)
		return;
	if (saving) {
		obs_data_t *props = obs_data_create();
		obs_data_array_t *array = obs_data_array_create();
		for (size_t i = 0; i < docks->size(); i++) {
			FTDock *d = (*docks)[i];
			obs_data_t *obj = obs_data_create();
			d->save_properties(obj);
			obs_data_set_string(obj, "name", d->name.c_str());
			obs_data_array_push_back(array, obj);
			obs_data_release(obj);
		}
		obs_data_set_array(props, "docks", array);
		obs_data_set_obj(save_data, SAVE_DATA_NAME, props);
		obs_data_array_release(array);
		obs_data_release(props);
	}

	else /* loading */ {
		if (docks)
			while (docks->size()) {
				(*docks)[docks->size() - 1]->close();
				delete (*docks)[docks->size() - 1];
			}

		obs_data_t *props = obs_data_get_obj(save_data, SAVE_DATA_NAME);
		if (!props) {
			blog(LOG_INFO, "save_load_ft_docks: creating default properties");
			props = obs_data_create();
		}

		obs_data_array_t *array = obs_data_get_array(props, "docks");
		size_t count = obs_data_array_count(array);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *obj = obs_data_array_item(array, i);
			FTDock::default_properties(obj);
			const char *name = obs_data_get_string(obj, "name");
			ft_dock_add(name, obj, false);
			obs_data_release(obj);
		}
		obs_data_array_release(array);
		obs_data_release(props);
	}
}

void ft_docks_init()
{
	if (docks)
		return;
	docks = new std::vector<FTDock *>;
	obs_frontend_add_event_callback(ft_docks_frontend_event_cb, NULL);
	dock_event_callback_registered = true;
	obs_frontend_add_save_callback(save_load_ft_docks, NULL);
	dock_save_callback_registered = true;

	new_dock_action = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction(obs_module_text("New FaithfulFrame Dock...")));
	QAction *action = new_dock_action.data();
	blog(LOG_INFO, "ft_docks_init: Adding face tracker dock menu action=%p", action);
	if (!action) {
		blog(LOG_WARNING, "ft_docks_init: could not create the dock menu action");
		return;
	}
	QWidget *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	dock_action_context = new QObject(main_window);
	QObject::connect(action, &QAction::triggered, dock_action_context.data(), [] {
		obs_data_t *props = obs_data_create();
		FTDock::default_properties(props);
		ft_dock_add(NULL, props, true);
		obs_data_release(props);
	});
}

void ft_docks_release()
{
	if (dock_event_callback_registered) {
		if (!dock_frontend_shutdown_started)
			obs_frontend_remove_event_callback(ft_docks_frontend_event_cb, NULL);
		dock_event_callback_registered = false;
	}
	if (dock_save_callback_registered) {
		obs_frontend_remove_save_callback(save_load_ft_docks, NULL);
		dock_save_callback_registered = false;
	}
	if (docks) {
		while (!docks->empty()) {
			FTDock *dock = docks->back();
			QPointer<FTDock> dock_lifetime(dock);
			const std::string id = "ft." + dock->name;
			obs_frontend_remove_dock(id.c_str());
			if (dock_lifetime) {
				dock_lifetime->close();
				delete dock_lifetime.data();
			}
		}
		delete docks;
		docks = NULL;
	}
	if (new_dock_action) {
		new_dock_action->setEnabled(false);
		new_dock_action->setVisible(false);
	}
	QObject *context = dock_action_context.data();
	dock_action_context.clear();
	if (context)
		delete context;
	new_dock_action.clear();
}

void FTDock::default_properties(obs_data_t *) {}

void FTDock::save_properties(obs_data_t *props)
{
	// Save indicates a source or a filter has been changed.
	scenesMayChanged();

	pthread_mutex_lock(&data->mutex);

	obs_data_t *prop = obs_source_get_settings(data->src_monitor);
	if (prop) {
		obs_data_set_obj(props, "monitor", prop);
		obs_data_release(prop);
	}

	pthread_mutex_unlock(&data->mutex);
}

void FTDock::load_properties(obs_data_t *props)
{
	pthread_mutex_lock(&data->mutex);

	if (data && data->src_monitor) {
		obs_data_t *prop = obs_data_get_obj(props, "monitor");
		if (prop) {
			obs_source_update(data->src_monitor, prop);
			obs_data_release(prop);
		}
	}

	pthread_mutex_unlock(&data->mutex);

	dataChanged();
}

void FTDock::removeDock()
{
	std::string id = "ft." + name;
	obs_frontend_remove_dock(id.c_str());
}

struct face_tracker_dock_s *face_tracker_dock_create()
{
	struct face_tracker_dock_s *data = (struct face_tracker_dock_s *)bzalloc(sizeof(struct face_tracker_dock_s));
	data->ref = 1;
	pthread_mutex_init(&data->mutex, NULL);
	return data;
}

void face_tracker_dock_destroy(struct face_tracker_dock_s *data)
{
	if (data->disp)
		obs_display_destroy(data->disp);
	data->disp = NULL;
	if (data->src_monitor)
		obs_source_release(data->src_monitor);
	pthread_mutex_destroy(&data->mutex);
	bfree(data);
}
