#include <QApplication>
#include <QMouseEvent>

#include <cstdio>

#include "face-tracker-dock-internal.hpp"
#include "face-tracker-widget.hpp"

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

	return 0;
}
