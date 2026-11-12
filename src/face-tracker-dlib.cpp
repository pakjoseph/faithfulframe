#include <obs-module.h>
#include "plugin-macros.generated.h"
#include "texture-object.h"
#include "face-tracker-dlib.h"
#include "faithfulframe-worker-log.hpp"

#include <dlib/image_processing/scan_fhog_pyramid.h>
#include <dlib/image_processing/correlation_tracker.h>
#include <dlib/image_processing.h>
#include <chrono>
#include <utility>

struct face_tracker_dlib_private_s
{
	std::shared_ptr<texture_object> tex;
	rect_s rect;
	dlib::correlation_tracker *tracker;
	int tracker_nc, tracker_nr;
	dlib::shape_predictor sp;
	dlib::full_object_detection shape;
	float last_scale;
	float score0;
	float pslr_max, pslr_min;
	bool need_restart;
	uint64_t last_ns;
	float scale_orig;
	int n_track;
	rectf_s upsize;
	char *landmark_detection_data;
	bool landmark_detection_data_updated;
	bool sp_available = false;
	uint64_t last_observation_ms = 0;

	face_tracker_dlib_private_s()
	{
		tracker = NULL;
		need_restart = false;
		tex = NULL;
		rect.score = 0.0f;
		landmark_detection_data = NULL;
		landmark_detection_data_updated = false;
	}
};

face_tracker_dlib::face_tracker_dlib()
{
	p = new face_tracker_dlib_private_s;
}

face_tracker_dlib::~face_tracker_dlib()
{
	bfree(p->landmark_detection_data);
	if (p->tracker)
		delete p->tracker;
	delete p;
}

void face_tracker_dlib::set_texture(std::shared_ptr<texture_object> &tex)
{
	p->tex = tex;
	p->n_track = 0;
}

void face_tracker_dlib::set_position(const rect_s &rect)
{
	if (!p->tex) {
		faithfulframe_worker_log(LOG_ERROR,
					 "face_tracker_dlib::set_position: texture was not set. rect=(%d %d %d %d %f)",
					 rect.x0, rect.y0, rect.x1, rect.y1, rect.score);
		return;
	}

	p->rect.x0 = rect.x0 / p->tex->scale;
	p->rect.y0 = rect.y0 / p->tex->scale;
	p->rect.x1 = rect.x1 / p->tex->scale;
	p->rect.y1 = rect.y1 / p->tex->scale;
	p->rect.score = 1.0f;
	p->need_restart = true;
	p->n_track = 0;
}

void face_tracker_dlib::set_upsize_info(const rectf_s &upsize)
{
	p->upsize = upsize;
}

void face_tracker_dlib::set_landmark_detection(const char *data_file_path)
{
	if (p->landmark_detection_data && data_file_path && strcmp(p->landmark_detection_data, data_file_path) == 0)
		return;

	bfree(p->landmark_detection_data);
	p->landmark_detection_data = NULL;
	if (data_file_path) {
		p->landmark_detection_data = bstrdup(data_file_path);
		p->landmark_detection_data_updated = true;
	}
}

template<typename Tx, typename Ta> inline Tx internal_division(Tx x0, Tx x1, Ta a0, Ta a1)
{
	return (x0 * a1 + x1 * a0) / (a0 + a1);
}

void face_tracker_dlib::track_main()
{
	std::shared_ptr<texture_object> tex = std::move(p->tex);
	if (!tex) {
		p->n_track = 0;
		return;
	}

	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
	bool observed = false;
	if (p->need_restart) {
		if (!p->tracker)
			p->tracker = new dlib::correlation_tracker();

		dlib::matrix<dlib::rgb_pixel> img;
		if (!tex->get_dlib_rgb_image(img))
			return;

		dlib::rectangle r(p->rect.x0, p->rect.y0, p->rect.x1, p->rect.y1);
		p->tracker->start_track(img, r);
		p->tracker_nc = img.nc();
		p->tracker_nr = img.nr();
		p->score0 = p->rect.score;
		p->need_restart = false;
		p->pslr_max = 0.0f;
		p->pslr_min = 1e9f;
		p->scale_orig = tex->scale;
		p->shape = dlib::full_object_detection();
		observed = true;
	} else if (tex->scale != p->scale_orig) {
		p->rect.score = 0.0f;
	} else {
		dlib::matrix<dlib::rgb_pixel> img;
		if (!tex->get_dlib_rgb_image(img))
			return;

		if (img.nc() != p->tracker_nc || img.nr() != p->tracker_nr) {
			faithfulframe_worker_log(
				LOG_ERROR,
				"face_tracker_dlib::track_main: cannot run correlation-tracker with different image size %dx%d, expected %dx%d",
				(int)img.nc(), (int)img.nr(), p->tracker_nc, p->tracker_nr);
			p->rect.score = 0;
			p->n_track += 1; // to return score=0
			return;
		}

		float s = p->tracker->update(img);
		if (s > p->pslr_max)
			p->pslr_max = s;
		if (s < p->pslr_min)
			p->pslr_min = s;
		dlib::rectangle r = p->tracker->get_position();
		p->rect.x0 = r.left() * tex->scale;
		p->rect.y0 = r.top() * tex->scale;
		p->rect.x1 = r.right() * tex->scale;
		p->rect.y1 = r.bottom() * tex->scale;
		s = p->pslr_max / p->pslr_min * ((ns - p->last_ns) * 1e-9f);
		p->rect.score = (p->rect.score /*+ 0.0f*s */) / (1.0f + s);
		p->n_track += 1;
		observed = true;

		if (p->landmark_detection_data) {
			if (p->landmark_detection_data_updated) {
				p->landmark_detection_data_updated = false;
				faithfulframe_worker_log(LOG_INFO, "loading file %s", p->landmark_detection_data);
				try {
					p->sp_available = false;
					dlib::deserialize(p->landmark_detection_data) >> p->sp;
					p->sp_available = true;
				} catch (...) {
					faithfulframe_worker_log(LOG_ERROR, "Failed to load file %s",
								 p->landmark_detection_data);
				}
			}

			dlib::rectangle r_face(
				internal_division(r.left(), r.right(), p->upsize.x0, p->upsize.x1 + 1.0f),
				internal_division(r.top(), r.bottom(), p->upsize.y0, p->upsize.y1 + 1.0f),
				internal_division(r.left(), r.right(), p->upsize.x0 + 1.0f, p->upsize.x1),
				internal_division(r.top(), r.bottom(), p->upsize.y0 + 1.0f, p->upsize.y1));

			if (p->sp_available)
				p->shape = p->sp(img, r_face);
			p->last_scale = tex->scale;
		}
	}
	p->last_ns = ns;
	if (observed)
		p->last_observation_ms = tex->captured_at_ms;
}

bool face_tracker_dlib::get_face(struct rect_s &rect, uint64_t &observed_at_ms)
{
	if (p->n_track > 0) {
		rect = p->rect;
		observed_at_ms = p->last_observation_ms;
		return true;
	} else
		return false;
}

bool face_tracker_dlib::get_landmark(std::vector<pointf_s> &results)
{
	if (p->shape.num_parts() > 0) {
		const auto &shape = p->shape;
		results.resize(shape.num_parts());

		for (unsigned long i = 0; i < shape.num_parts(); i++) {
			const dlib::point pnt = shape.part(i);
			results[i].x = (float)pnt.x() * p->last_scale;
			results[i].y = (float)pnt.y() * p->last_scale;
		}

		return true;
	} else
		return false;
}
