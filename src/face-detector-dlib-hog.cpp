#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include "plugin-macros.generated.h"
#include "face-detector-dlib-hog.h"
#include "texture-object.h"
#include "faithfulframe-worker-log.hpp"

#include <dlib/image_processing/frontal_face_detector.h>
#include <cmath>
#include <limits>
#include <utility>

#define MAX_ERROR 2

struct face_detector_dlib_private_s
{
	std::shared_ptr<texture_object> tex;
	std::vector<rect_s> rects;
	dlib::frontal_face_detector detector;
	bool detector_loaded = false;
	bool has_error = false;
	std::string model_filename;
	int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
	int n_error = 0;
	face_detector_dlib_private_s() {}
	~face_detector_dlib_private_s() {}
};

face_detector_dlib_hog::face_detector_dlib_hog()
{
	p = new face_detector_dlib_private_s;
}

face_detector_dlib_hog::~face_detector_dlib_hog()
{
	// The worker calls detect_main() through this derived object and accesses p.
	// Stop it before releasing derived state; the base destructor's stop() runs
	// only after this destructor body has completed.
	stop();
	delete p;
}

void face_detector_dlib_hog::set_texture(std::shared_ptr<texture_object> &tex, int crop_l, int crop_r, int crop_t,
					 int crop_b)
{
	p->tex = tex;
	p->crop_l = crop_l;
	p->crop_r = crop_r;
	p->crop_t = crop_t;
	p->crop_b = crop_b;
}

void face_detector_dlib_hog::detect_main()
{
	// A failed frame must not leave the previous detections looking current.
	p->rects.clear();
	std::shared_ptr<texture_object> tex = std::move(p->tex);
	set_result_epoch(tex ? tex->detection_epoch : 0);
	if (!tex)
		return;

	dlib::matrix<dlib::rgb_pixel> img;
	if (!tex->get_dlib_rgb_image(img))
		return;

	int x0 = 0, y0 = 0;
	if (p->crop_l > 0 || p->crop_r > 0 || p->crop_t > 0 || p->crop_b > 0) {
		dlib::matrix<dlib::rgb_pixel> img_crop;
		x0 = (int)(p->crop_l / tex->scale);
		int x1 = img.nc() - (int)(p->crop_r / tex->scale);
		y0 = (int)(p->crop_t / tex->scale);
		int y1 = img.nr() - (int)(p->crop_b / tex->scale);
		if (x1 - x0 < 80 || y1 - y0 < 80) {
			if (p->n_error++ < MAX_ERROR)
				faithfulframe_worker_log(
					LOG_ERROR, "too small image: %dx%d cropped left=%d right=%d top=%d bottom=%d",
					(int)img.nc(), (int)img.nr(), p->crop_l, p->crop_r, p->crop_t, p->crop_b);
			return;
		} else if (p->n_error) {
			p->n_error--;
		}
		img_crop.set_size(y1 - y0, x1 - x0);
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				img_crop(y - y0, x - x0) = img(y, x);
			}
		}
		img = img_crop;
	}
	if (img.nc() < 80 || img.nr() < 80) {
		if (p->n_error++ < MAX_ERROR)
			faithfulframe_worker_log(LOG_ERROR, "too small image: %dx%d", (int)img.nc(), (int)img.nr());
		return;
	} else if (p->n_error) {
		p->n_error--;
	}

	if (!p->detector_loaded) {
		p->detector_loaded = true;
		try {
			faithfulframe_worker_log(LOG_INFO, "loading the configured dlib HOG detector model");
			dlib::deserialize(p->model_filename.c_str()) >> p->detector;
			p->has_error = false;
		} catch (...) {
			faithfulframe_worker_log(LOG_ERROR, "could not load the configured dlib HOG detector model");
			p->has_error = true;
		}
	}

	if (!p->has_error) {
		std::vector<std::pair<double, dlib::rectangle>> dets;
		p->detector(img, dets);
		p->rects.resize(dets.size());
		for (size_t i = 0; i < dets.size(); i++) {
			rect_s &r = p->rects[i];
			const dlib::rectangle &rect = dets[i].second;
			r.x0 = (rect.left() + x0) * tex->scale;
			r.y0 = (rect.top() + y0) * tex->scale;
			r.x1 = (rect.right() + x0) * tex->scale;
			r.y1 = (rect.bottom() + y0) * tex->scale;
			const double confidence = dets[i].first;
			// Keep dlib's detection margin so tracker attenuation can distinguish
			// weak detections. A malformed or unrepresentable score fails closed.
			r.score = std::isfinite(confidence) && confidence > 0.0 &&
						  confidence <= std::numeric_limits<float>::max()
					  ? static_cast<float>(confidence)
					  : 0.0f;
		}
	}
}

void face_detector_dlib_hog::get_faces(std::vector<struct rect_s> &rects)
{
	rects = p->rects;
}

void face_detector_dlib_hog::set_model(const char *filename)
{
	if (p->model_filename != filename) {
		p->model_filename = filename;
		p->detector_loaded = false;
	}
}
