#include <obs-module.h>
#include <obs-frontend-api.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <QAction>
#include <QMainWindow>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QRectF>
#include <QTimer>
#include <QSignalBlocker>
#include <QVariantMap>
#include "sanctuary-style.hpp"
#include "plugin-macros.generated.h"
#include "face-tracker-dock.hpp"
#include "face-tracker-widget.hpp"
#include "face-tracker-dock-internal.hpp"
#ifdef ENABLE_RUST_CORE_BRIDGE
#include "faithfulframe-preview-geometry.hpp"
#include "faithfulframe_ffi.h"
#endif

#define SAVE_DATA_NAME "face-tracker-dock"
#define OBJ_NAME_SUFFIX "_ft_dock"

void FTDock::closeEvent(QCloseEvent *event)
{
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

#ifdef ENABLE_RUST_CORE_BRIDGE
struct faithfulframe_safe_preview
{
	bool enabled = false;
	bool available = false;
	bool target_active = false;
	bool signal_connected = false;
	uint32_t state = 0;
	uint32_t reason = 0;
	uint32_t command_result = 0;
	uint32_t rejection_reason = 0;
	uint32_t source_width = 0;
	uint32_t source_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	int64_t status_snapshot_age_ms = -1;
	int64_t loss_remaining_ms = -1;
	uint64_t executor_queue_depth = 0;
	uint64_t executor_queue_high_water = 0;
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
	calldata_get_bool(&cd, "target_active", &preview.target_active);
	calldata_get_bool(&cd, "signal_connected", &preview.signal_connected);
	long long snapshot_age_ms = -1;
	if (calldata_get_int(&cd, "status_snapshot_age_ms", &snapshot_age_ms))
		preview.status_snapshot_age_ms = snapshot_age_ms;
	calldata_get_int(&cd, "loss_remaining_ms", &preview.loss_remaining_ms);
	read_calldata_int(&cd, "executor_queue_depth", preview.executor_queue_depth);
	read_calldata_int(&cd, "executor_queue_high_water", preview.executor_queue_high_water);
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

static bool send_safe_command(obs_source_t *source, uint32_t kind, const QVariantMap *candidate = nullptr)
{
	faithfulframe_safe_preview preview;
	if (!read_safe_preview(source, preview) || !preview.enabled || !preview.available)
		return false;
	if (candidate) {
		// Selection uses the identity and versions attached to the short-lived
		// preview item; a stale item is intentionally left for Rust to reject.
		preview.source_id = candidate->value("source_id").toULongLong();
		preview.session_id = candidate->value("session_id").toULongLong();
		preview.status_version = candidate->value("status_version").toULongLong();
		preview.observation_version = candidate->value("observation_version").toULongLong();
		preview.source_generation = candidate->value("source_generation").toULongLong();
		preview.session_generation = candidate->value("session_generation").toULongLong();
		preview.stream_generation = candidate->value("stream_generation").toULongLong();
		preview.geometry_generation = candidate->value("geometry_generation").toULongLong();
		preview.tracker_generation = candidate->value("tracker_generation").toULongLong();
	}

	proc_handler_t *handler = obs_source_get_proc_handler(source);
	if (!handler)
		return false;
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
	calldata_get_bool(&cd, "queued", &queued);
	calldata_free(&cd);
	return called && queued;
}

static QString safe_state_text(const faithfulframe_safe_preview &preview)
{
	if (!preview.available)
		return "FaithfulFrame safe tracking: waiting for a valid source session";
	if (preview.command_result == 2) {
		if (preview.rejection_reason == 1)
			return "Selection or command expired; choose again from the current preview";
		if (preview.rejection_reason == 7)
			return "The tracking session has closed; reselect the camera in source settings";
		return "Command was rejected without changing framing; review status and try a valid action";
	}
	if (!preview.signal_connected)
		return "Camera disconnected; restore the camera, then select the speaker again";
	if (!preview.target_active)
		return "Camera inactive; activate its scene, then select the speaker again";
	switch (preview.state) {
	case FF_STATE_FROZEN:
		if (preview.reason == 8)
			return "Frozen at wide framing; press Resume when ready";
		return "Frozen; press Resume when ready";
	case FF_STATE_MANUAL:
		if (preview.reason == 8)
			return "Wide framing; select a face to follow again";
		return "Manual; select a face before following";
	case FF_STATE_SEARCHING:
		if (preview.loss_remaining_ms >= 0)
			return QString("Target briefly lost; holding framing. Same-speaker reacquisition: %1 s; then select again.")
				.arg(preview.loss_remaining_ms / 1000.0, 0, 'f', 1);
		return "Searching; holding the last framing";
	case FF_STATE_LOCKED:
		return "Following the selected speaker";
	case FF_STATE_LOST:
		return "Target lost; select a face or reset wide";
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
	if (!obs_frontend_add_dock_by_id(id.c_str(), dock->name.c_str(), static_cast<QWidget *>(dock)))
		return;

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
	const char *source_name;
	const char *filter_name;
};

static bool init_target_selector_compare_name(struct init_target_selector_s *ctx, const char *source_name,
					      const char *filter_name)
{
	if (strcmp(ctx->source_name, source_name) != 0)
		return false;

	// both expect source
	if (!ctx->filter_name && !filter_name)
		return true;

	// either one expects source
	if (!ctx->filter_name || !filter_name)
		return false;

	return strcmp(ctx->filter_name, filter_name) == 0;
}

static void init_target_selector_cb_add(struct init_target_selector_s *ctx, obs_source_t *source, obs_source_t *filter)
{
	QString text;
	QList<QVariant> val;

	const char *name = obs_source_get_name(source);
	const char *filter_name = NULL;
	text = QString::fromUtf8(name);
	val.append(QByteArray(name));

	if (filter) {
		filter_name = obs_source_get_name(filter);
		text += " / ";
		text += QString::fromUtf8(filter_name);
		val.append(QByteArray(filter_name));
	}

	if (ctx->index < ctx->q->count()) {
		ctx->q->setItemText(ctx->index, text);
		ctx->q->setItemData(ctx->index, val);
	} else
		ctx->q->insertItem(ctx->index, text, val);

	if (ctx->source_name) {
		if (init_target_selector_compare_name(ctx, name, filter_name))
			ctx->q->setCurrentIndex(ctx->index);
	}

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

static void init_target_selector(QComboBox *q, const char *source_name = NULL, const char *filter_name = NULL)
{
	QString current = q->currentText();

	if (filter_name && !*filter_name)
		filter_name = NULL;

	init_target_selector_s ctx = {q, 0, source_name, filter_name};
	obs_enum_scenes(init_target_selector_cb_source, &ctx);
	obs_enum_sources(init_target_selector_cb_source, &ctx);

	while (q->count() > ctx.index)
		q->removeItem(ctx.index);

	if (current.length() && !source_name) {
		int ix = q->findText(current);
		if (ix >= 0)
			q->setCurrentIndex(ix);
	}
}

void FTDock::checkTargetSelector()
{
	init_target_selector(targetSelector);
}

void FTDock::frontendEvent_cb(enum obs_frontend_event event, void *private_data)
{
	auto *dock = static_cast<FTDock *>(private_data);
	dock->frontendEvent(event);
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
	mainLayout->addWidget(pauseButton);
	connect(pauseButton, &QPushButton::clicked, this, &FTDock::pauseButtonClicked);

	resetButton = new QPushButton(obs_module_text("Reset"), this);
	mainLayout->addWidget(resetButton);
	connect(resetButton, &QPushButton::clicked, this, &FTDock::resetButtonClicked);

	autoFollowButton = new QPushButton("Auto Follow", this);
	autoFollowButton->setAccessibleName("Enable automatic speaker following");
	mainLayout->addWidget(autoFollowButton);
	connect(autoFollowButton, &QPushButton::clicked, this, &FTDock::autoFollowButtonClicked);

	manualOverrideButton = new QPushButton("Manual Override", this);
	manualOverrideButton->setAccessibleName("Stop automatic following and use manual framing");
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
	connect(performanceToggle, &QToolButton::toggled, this, [this](bool expanded) {
		performanceToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		performanceLabel->setVisible(expanded && performanceToggle->isVisible());
	});
#ifndef ENABLE_RUST_CORE_BRIDGE
	autoFollowButton->hide();
	manualOverrideButton->hide();
	candidateSelector->hide();
	selectCandidateButton->hide();
	safeStatusLabel->hide();
	performanceToggle->hide();
	performanceLabel->hide();
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

	obs_frontend_add_event_callback(frontendEvent_cb, this);
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
				    manualOverrideButton, selectCandidateButton}) {
		if (button)
			button->setStyleSheet(button_style);
	}
	for (QPushButton *button :
	     {pauseButton, resetButton, autoFollowButton, manualOverrideButton, selectCandidateButton}) {
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
	// OBS tears down the frontend callback registry as it dispatches EXIT.
	if (!frontend_shutdown_started)
		obs_frontend_remove_event_callback(frontendEvent_cb, this);

	if (source_sh) {
		signal_handler_disconnect(source_sh, "state_changed", onStateChanged, this);
		source_sh = NULL;
	}

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

void FTDock::frontendEvent(enum obs_frontend_event event)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		frontend_shutdown_started = true;

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || event == OBS_FRONTEND_EVENT_EXIT) {
		if (docks)
			while (docks->size()) {
				(*docks)[docks->size() - 1]->close();
				delete (*docks)[docks->size() - 1];
			}
	}
}

void FTDock::targetSelectorChanged()
{
	if (updating_widget)
		return;

	updateState();
}

OBSSource FTDock::get_source()
{
	OBSSource target;
	QList<QVariant> data = targetSelector->currentData().toList();

	for (int i = 0; i < data.count(); i++) {
		const char *name = data[i].toByteArray().constData();
		if (i == 0) {
			target = obs_get_source_by_name(name);
			obs_source_release(target);
		} else if (i == 1) {
			target = obs_source_get_filter_by_name(target, name);
			obs_source_release(target);
		}
	}

	return target;
}

static bool is_filter(QComboBox *targetSelector)
{
	return targetSelector->currentData().toList().count() == 2;
}

static inline void set_monitor(obs_source_t *monitor, const QList<QVariant> &target_data)
{
	blog(LOG_INFO, "set_monitor monitor=%p", monitor);
	OBSData data = obs_data_create();
	obs_data_release(data);

	if (target_data.count() < 1)
		return;

	obs_data_set_string(data, "source_name", target_data[0].toByteArray().constData());

	obs_data_set_string(data, "filter_name",
			    target_data.count() > 1 ? target_data[1].toByteArray().constData() : "");

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
	if (is_paused)
		pauseButton->setText(obs_module_text("Resume"));
	else
		pauseButton->setText(obs_module_text("Pause"));
}

void FTDock::onStateChanged(void *data, calldata_t *)
{
	FTDock *dock = (FTDock *)data;
	QMetaObject::invokeMethod(dock, "updateState", Qt::QueuedConnection);
}

void FTDock::updateState()
{
	OBSSource target = get_source();
	proc_handler_t *ph = obs_source_get_proc_handler(target);
	if (!ph) {
		autoFollowButton->hide();
		manualOverrideButton->hide();
		candidateSelector->hide();
		selectCandidateButton->hide();
		safeStatusLabel->hide();
		performanceToggle->hide();
		performanceLabel->hide();
		if (source_sh) {
			signal_handler_disconnect(source_sh, "state_changed", onStateChanged, this);
			source_sh = NULL;
		}
		return;
	}

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
	enableButton->setVisible(!safe_mode);
	notrackButton->setVisible(!safe_mode);
	if (safe_mode) {
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
		safeStatusLabel->setText(QString("Tracking source: %1\n%2\n%3\nCurrent zoom: %4×\n%5")
						 .arg(tracking_source)
						 .arg(safe_preview.framing_preset)
						 .arg(safe_state_text(safe_preview))
						 .arg(safe_preview_zoom(safe_preview), 0, 'f', 1)
						 .arg(resolution));
		const QString snapshot_age = safe_preview.status_snapshot_age_ms < 0
						     ? QString("unavailable")
						     : QString("%1 ms").arg(safe_preview.status_snapshot_age_ms);
		performanceLabel->setText(
			QString("Snapshot: %1 | queue: %2 / %3 | wait: %4 / %5 µs | policy: %6 / %7 µs | "
				"dlib: %8 runs, %9 / %10 µs | accepted %11 · rejected %12 · coalesced %13")
				.arg(snapshot_age)
				.arg(static_cast<qulonglong>(safe_preview.executor_queue_depth))
				.arg(static_cast<qulonglong>(safe_preview.executor_queue_high_water))
				.arg(static_cast<qulonglong>(safe_preview.executor_last_queue_wait_us))
				.arg(static_cast<qulonglong>(safe_preview.executor_max_queue_wait_us))
				.arg(static_cast<qulonglong>(safe_preview.executor_last_processing_us))
				.arg(static_cast<qulonglong>(safe_preview.executor_max_processing_us))
				.arg(static_cast<qulonglong>(safe_preview.detector_work_count))
				.arg(static_cast<qulonglong>(safe_preview.detector_last_work_us))
				.arg(static_cast<qulonglong>(safe_preview.detector_max_work_us))
				.arg(static_cast<qulonglong>(safe_preview.executor_accepted_work))
				.arg(static_cast<qulonglong>(safe_preview.executor_rejected_work))
				.arg(static_cast<qulonglong>(safe_preview.executor_coalesced_work)));
		set_pause_button(pauseButton, safe_preview.state == FF_STATE_FROZEN);
		pauseButton->setEnabled(safe_preview.available);
		resetButton->setText("Reset to Wide");
		resetButton->setEnabled(safe_preview.available);
		autoFollowButton->setEnabled(safe_preview.available && (safe_preview.state == FF_STATE_MANUAL ||
									safe_preview.state == FF_STATE_LOST));
		manualOverrideButton->setEnabled(safe_preview.available && safe_preview.state != FF_STATE_FROZEN &&
						 safe_preview.state != FF_STATE_MANUAL);
		const QVariantMap previous_candidate = candidateSelector->currentData().toMap();
		const qulonglong previous_handle = previous_candidate.value("handle").toULongLong();
		QSignalBlocker blocker(candidateSelector);
		candidateSelector->clear();
		int visible_candidate_count = 0;
		for (const QVariantMap &candidate : safe_preview.candidates) {
			QRectF visible;
			if (!candidate_output_rect(safe_preview, candidate, visible))
				continue;
			const int x = qRound(visible.center().x() * 100.0);
			const int y = qRound(visible.center().y() * 100.0);
			candidateSelector->addItem(QString("Visible face %1 · preview %2%%, %3%%")
							   .arg(++visible_candidate_count)
							   .arg(x)
							   .arg(y),
						   candidate);
			if (candidate.value("handle").toULongLong() == previous_handle)
				candidateSelector->setCurrentIndex(candidateSelector->count() - 1);
		}
		if (!visible_candidate_count)
			candidateSelector->addItem("No fresh face candidates");
		selectCandidateButton->setEnabled(safe_preview.available && visible_candidate_count > 0 &&
						  (safe_preview.state == FF_STATE_MANUAL ||
						   safe_preview.state == FF_STATE_SEARCHING ||
						   safe_preview.state == FF_STATE_LOST));
	}
#else
	UNUSED_PARAMETER(safe_mode);
#endif
	if (!safe_mode) {
		resetButton->setText(obs_module_text("Reset"));
		resetButton->setEnabled(true);
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

	if (!data)
		return;

	pthread_mutex_lock(&data->mutex);

	if (data->src_monitor)
		set_monitor(data->src_monitor, targetSelector->currentData().toList());

	pthread_mutex_unlock(&data->mutex);
}

void FTDock::updateWidget()
{
	if (updating_widget)
		return;
	updating_widget = true;
	pthread_mutex_lock(&data->mutex);

	if (data->src_monitor) {
		obs_data_t *props = obs_source_get_settings(data->src_monitor);

		const char *source_name = obs_data_get_string(props, "source_name");
		const char *filter_name = obs_data_get_string(props, "filter_name");
		init_target_selector(targetSelector, source_name, filter_name);

		set_enable_button(enableButton, is_filter(targetSelector), obs_source_enabled(get_source()));

		bool notrack = obs_data_get_bool(props, "notrack");
		notrackButton->setCheckState(notrack ? Qt::Checked : Qt::Unchecked);

		obs_data_release(props);
	}

	pthread_mutex_unlock(&data->mutex);
	updating_widget = false;

	updateState();
}

void FTDock::pauseButtonClicked(bool checked)
{
	if (in_updateState)
		return;

	OBSSource target = get_source();
#ifdef ENABLE_RUST_CORE_BRIDGE
	faithfulframe_safe_preview safe_preview;
	if (read_safe_preview(target, safe_preview) && safe_preview.enabled) {
		if (!send_safe_command(target, checked ? FF_COMMAND_FREEZE : FF_COMMAND_RESUME))
			safeStatusLabel->setText(
				"Command was stale or unavailable; wait for status to refresh and try again");
		return;
	}
#endif
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
		if (!send_safe_command(target, FF_COMMAND_RESET_TO_WIDE))
			safeStatusLabel->setText("Reset command was stale or unavailable; try again");
		return;
	}
#endif
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
	if (!send_safe_command(target, FF_COMMAND_ENABLE_AUTO_FOLLOW))
		safeStatusLabel->setText(
			"Auto Follow was stale or unavailable; wait for status to refresh and try again");
#endif
}

void FTDock::manualOverrideButtonClicked()
{
#ifdef ENABLE_RUST_CORE_BRIDGE
	OBSSource target = get_source();
	if (!send_safe_command(target, FF_COMMAND_MANUAL_OVERRIDE))
		safeStatusLabel->setText(
			"Manual Override was stale or unavailable; wait for status to refresh and try again");
#endif
}

void FTDock::selectCandidateButtonClicked()
{
#ifdef ENABLE_RUST_CORE_BRIDGE
	const QVariantMap candidate = candidateSelector->currentData().toMap();
	if (candidate.isEmpty()) {
		safeStatusLabel->setText("No fresh candidate is available. Wait for a face to appear in the preview.");
		return;
	}
	OBSSource target = get_source();
	if (!send_safe_command(target, FF_COMMAND_SELECT_CANDIDATE, &candidate))
		safeStatusLabel->setText(
			"Selection expired or changed. Choose the face again from the current preview.");
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
	if (preview.state != FF_STATE_MANUAL && preview.state != FF_STATE_SEARCHING && preview.state != FF_STATE_LOST)
		return;

	// FTWidget and Qt mouse events use logical pixels. The monitor's rendered
	// preview uses the same aspect-fit transform, so normalizing inside the
	// letterboxed image accounts for both HiDPI scaling and padding.
	double x = 0.0, y = 0.0;
	if (!ff_preview_widget_point(position.x(), position.y(), ftWidget->width(), ftWidget->height(),
				     preview.output_width, preview.output_height, x, y))
		return;

	QVariantMap hit;
	int hits = 0;
	for (const QVariantMap &candidate : preview.candidates) {
		QRectF visible;
		if (!candidate_output_rect(preview, candidate, visible) || !visible.contains(QPointF(x, y)))
			continue;
		hit = candidate;
		++hits;
	}
	if (hits != 1) {
		safeStatusLabel->setText(hits > 1
						 ? "Overlapping faces are ambiguous; choose one from the candidate list"
						 : "Click inside one visible face to select its speaker");
		return;
	}
	if (!send_safe_command(target, FF_COMMAND_SELECT_CANDIDATE, &hit))
		safeStatusLabel->setText("Selection expired or changed. Click the face again in the current preview.");
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

	QList<QVariant> data = targetSelector->currentData().toList();
	if (data.count() < 1)
		return;

	const char *name = data[0].toByteArray().constData();

	obs_source_t *target = obs_get_source_by_name(name);
	if (!target) {
		blog(LOG_ERROR, "Cannot find the target for name='%s'", name);
		return;
	}

	if (data.count() == 1)
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
	docks = new std::vector<FTDock *>;
	obs_frontend_add_save_callback(save_load_ft_docks, NULL);

	QAction *action = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction(obs_module_text("New Face Tracker Dock...")));
	blog(LOG_INFO, "ft_docks_init: Adding face tracker dock menu action=%p", action);
	auto cb = [] {
		obs_data_t *props = obs_data_create();
		FTDock::default_properties(props);
		ft_dock_add(NULL, props, true);
		obs_data_release(props);
	};
	QAction::connect(action, &QAction::triggered, cb);
}

void ft_docks_release()
{
	delete docks;
	docks = NULL;
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
	obs_display_destroy(data->disp);
	data->disp = NULL;
	obs_source_release(data->src_monitor);
	pthread_mutex_destroy(&data->mutex);
	bfree(data);
}
