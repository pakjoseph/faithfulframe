#pragma once

#include <atomic>
#include <deque>
#include <cstdint>
#include <string>
#include "face-tracker-base.h"

class face_tracker_manager {
public:
	enum detector_engine_e {
		engine_dlib_hog = 0,
		engine_dlib_cnn = 1,
		engine_uninitialized = -1,
	};

	struct tracker_rect_s
	{
		uint64_t candidate_handle;
		uint64_t continuity_token;
		uint64_t observed_at_ms;
		rect_s rect;
		rectf_s crop_rect;
		std::vector<pointf_s> landmark;
	};

	struct tracker_inst_s
	{
		uint64_t candidate_handle;
		uint64_t continuity_token;
		class face_tracker_base *tracker;
		uint64_t observed_at_ms;
		rect_s rect;
		rectf_s crop_tracker; // crop corresponding to current processing image
		rectf_s crop_rect;    // crop corresponding to rect
		std::vector<pointf_s> landmark;
		float att;
		float score_first;
		std::shared_ptr<texture_object> seed_texture;
		enum tracker_state_e {
			tracker_state_init = 0,
			tracker_state_reset_texture, // texture has been set, position is not set.
			tracker_state_constructing, // texture and positions have been set, starting to construct correlation_tracker.
			tracker_state_first_track, // correlation_tracker has been prepared, running 1st tracking
			tracker_state_available,   // 1st tracking was done, `rect` is available, can accept next frame.
			tracker_state_ending,
		} state;
		int tick_cnt;
	};

public: // properties
	float upsize_l, upsize_r, upsize_t, upsize_b;
	std::atomic<float> scale{0.0f};
	std::atomic<bool> reset_requested{false};
	float tracking_threshold;
	enum detector_engine_e detector_engine = engine_uninitialized;
	std::string detector_dlib_hog_model;
	std::string detector_dlib_cnn_model;
	int detector_crop_l, detector_crop_r, detector_crop_t, detector_crop_b;
	char *landmark_detection_data;

public: // realtime status
	rectf_s crop_cur;
	int tick_cnt;

public: // results
	std::vector<rect_s> detect_rects;
	std::vector<tracker_rect_s> tracker_rects;

public: /* not sure they are necessary to be public */
	class face_detector_base *detect;
	int detect_tick;

	// TODO: Just have two pairs
	std::deque<struct tracker_inst_s> trackers;
	std::deque<struct tracker_inst_s> trackers_idlepool;
	uint64_t next_candidate_handle;
	std::atomic<uint64_t> detection_epoch{1};
	std::atomic<uint64_t> detector_input_epoch{0};

private:
	int next_tick_stage_to_detector;
	bool detector_in_progress;

public:
	face_tracker_manager();
	virtual ~face_tracker_manager();
	void tick(float second);
	void post_render(bool allow_detector_work = true);
	void update(obs_data_t *settings);
	static void get_properties(obs_properties_t *);
	static void get_defaults(obs_data_t *settings);

protected:
	virtual std::shared_ptr<texture_object> get_cvtex() = 0;

private:
	inline void retire_tracker(int ix);
	inline bool is_low_confident(const tracker_inst_s &t, float th1);
	void remove_duplicated_tracker();
	void attenuate_tracker();
	void copy_detector_to_tracker();
	void stage_to_detector(std::shared_ptr<texture_object> &frame, bool &capture_attempted);
	int stage_surface_to_tracker(struct tracker_inst_s &t, std::shared_ptr<texture_object> &frame,
				     bool &capture_attempted);
	void stage_to_trackers(std::shared_ptr<texture_object> &frame, bool &capture_attempted);
	uint64_t allocate_candidate_handle();
	class face_tracker_base *take_idle_tracker();
};
