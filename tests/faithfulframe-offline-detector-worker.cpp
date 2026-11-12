// Reuse the production worker implementation with logging disabled so the
// standalone replay tool does not require an initialized OBS process.
#define FAITHFULFRAME_DETECTOR_WORKER_STANDALONE 1
#include "../src/face-detector-base.cpp"
