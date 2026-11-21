#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>

#include <cstdio>

#include "faithfulframe-no-wheel-combo-box.hpp"
#include "face-tracker-dock-internal.hpp"
#include "face-tracker-widget.hpp"

const char *obs_module_text(const char *lookup_string)
{
	return lookup_string;
}

void face_tracker_dock_destroy(face_tracker_dock_s *data)
{
	pthread_mutex_destroy(&data->mutex);
	delete data;
}

void faithfulframe_monitor_begin_render(obs_source_t *) {}

bool faithfulframe_monitor_get_render_stamp(obs_source_t *, faithfulframe_preview_render_stamp &)
{
	return false;
}

static faithfulframe_preview_render_stamp make_stamp()
{
	faithfulframe_preview_render_stamp stamp{};
	stamp.valid = true;
	stamp.source_id = 11;
	stamp.session_id = 12;
	stamp.status_version = 13;
	stamp.observation_version = 14;
	stamp.source_generation = 15;
	stamp.session_generation = 16;
	stamp.stream_generation = 17;
	stamp.geometry_generation = 18;
	stamp.tracker_generation = 19;
	stamp.candidate_preview_revision = 20;
	stamp.candidate_earliest_expiry_ms = 21;
	stamp.display_width = 640;
	stamp.display_height = 360;
	stamp.output_width = 1920;
	stamp.output_height = 1080;
	stamp.crop = {0.1f, 0.2f, 0.7f, 0.6f};
	stamp.viewport = {0.0f, 0.125f, 1.0f, 0.75f};
	stamp.full_source = false;
	return stamp;
}

int main(int argc, char **argv)
{
	QApplication application(argc, argv);
	auto *data = new face_tracker_dock_s{};
	if (pthread_mutex_init(&data->mutex, nullptr) != 0) {
		delete data;
		std::fprintf(stderr, "failed to initialize preview-widget test mutex\n");
		return 1;
	}
	const auto expected_stamp = make_stamp();
	data->preview_render_stamp = expected_stamp;
	FTWidget widget(data, nullptr);
	int click_count = 0;
	QPointF clicked_position;
	faithfulframe_preview_render_stamp clicked_stamp{};
	QObject::connect(&widget, &FTWidget::previewClicked,
			 [&](const QPointF &position, const faithfulframe_preview_render_stamp &stamp) {
				 ++click_count;
				 clicked_position = position;
				 clicked_stamp = stamp;
			 });

	QMouseEvent left_click(QEvent::MouseButtonPress, QPointF(17.25, 23.5), Qt::LeftButton, Qt::LeftButton,
			       Qt::NoModifier);
	QApplication::sendEvent(&widget, &left_click);
	if (!left_click.isAccepted() || click_count != 1 || clicked_position != QPointF(17.25, 23.5) ||
	    !faithfulframe_preview_render_stamp_matches(expected_stamp, clicked_stamp)) {
		std::fprintf(stderr, "left click did not publish its logical position and rendered snapshot version\n");
		return 2;
	}

	QMouseEvent right_click(QEvent::MouseButtonPress, QPointF(31.0, 9.0), Qt::RightButton, Qt::RightButton,
				Qt::NoModifier);
	QApplication::sendEvent(&widget, &right_click);
	if (click_count != 1) {
		std::fprintf(stderr, "non-left click emitted a speaker-selection request\n");
		return 3;
	}

	pthread_mutex_lock(&data->mutex);
	QMouseEvent contended_click(QEvent::MouseButtonPress, QPointF(8.0, 12.0), Qt::LeftButton, Qt::LeftButton,
				    Qt::NoModifier);
	QApplication::sendEvent(&widget, &contended_click);
	pthread_mutex_unlock(&data->mutex);
	if (!contended_click.isAccepted() || click_count != 2 || clicked_stamp.valid) {
		std::fprintf(stderr, "preview click blocked or reused a stamp while render data was contended\n");
		return 4;
	}

	QWidget keyboard_host;
	FaithfulFrameNoWheelComboBox candidate_selector(&keyboard_host);
	QPushButton select_candidate(&keyboard_host);
	candidate_selector.addItem("Choose a detected face", QVariant{});
	candidate_selector.addItem("Visible face 1", QVariant::fromValue<qulonglong>(42));
	candidate_selector.setGeometry(0, 0, 180, 32);
	select_candidate.setGeometry(0, 40, 180, 32);
	candidate_selector.setCurrentIndex(0);
	keyboard_host.show();
	candidate_selector.setFocus(Qt::TabFocusReason);
	application.processEvents();
	QKeyEvent next_candidate(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
	QApplication::sendEvent(&candidate_selector, &next_candidate);
	QKeyEvent next_candidate_release(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
	QApplication::sendEvent(&candidate_selector, &next_candidate_release);
	if (!candidate_selector.hasFocus() || !next_candidate.isAccepted() || candidate_selector.currentIndex() != 1 ||
	    candidate_selector.currentData().toULongLong() != 42) {
		std::fprintf(stderr, "arrow-key navigation did not select the highlighted face candidate\n");
		return 5;
	}

	QKeyEvent tab_to_select(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
	QApplication::sendEvent(&candidate_selector, &tab_to_select);
	QKeyEvent tab_to_select_release(QEvent::KeyRelease, Qt::Key_Tab, Qt::NoModifier);
	QApplication::sendEvent(&candidate_selector, &tab_to_select_release);
	int keyboard_selection_count = 0;
	QObject::connect(&select_candidate, &QPushButton::clicked,
			 [&keyboard_selection_count] { ++keyboard_selection_count; });
	QKeyEvent confirm_selection(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
	QApplication::sendEvent(&select_candidate, &confirm_selection);
	QKeyEvent confirm_selection_release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
	QApplication::sendEvent(&select_candidate, &confirm_selection_release);
	if (!tab_to_select.isAccepted() || !select_candidate.hasFocus() || keyboard_selection_count != 1) {
		std::fprintf(stderr, "Space did not activate the highlighted-candidate selection control\n");
		return 6;
	}

	return 0;
}
