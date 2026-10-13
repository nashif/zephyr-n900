/* N900 phone demo: main camera */
#ifndef CAMERA_H_
#define CAMERA_H_

#include <stdbool.h>
#include <stdint.h>

#define CAM_PREVIEW_W 480
#define CAM_PREVIEW_H 360
#define CAM_PHOTO_W   640
#define CAM_PHOTO_H   480
#define CAM_PHOTOS    6

enum cam_focus_state {
	CAM_FOCUS_IDLE,
	CAM_FOCUS_SCANNING,
	CAM_FOCUS_LOCKED,
};

struct cam_status {
	bool running;
	bool sensor_ok;
	uint32_t fps;
	uint32_t exposure;
	uint32_t gain_x10;
	enum cam_focus_state focus;
	uint16_t lens;
	bool shooting;
	int photos;      /* photos taken so far */
	int last_photo;  /* slot of the newest photo, -1 if none */
};

/* Requests, handled by the camera thread */
void cam_init(void);
void cam_start(void);
void cam_stop(void);
void cam_focus(void);
void cam_shoot(bool flash);

/* The newest preview frame (XRGB8888), and its sequence number */
const uint32_t *cam_preview(uint32_t *seq);
/* Tells the camera thread which preview buffer the UI is showing */
void cam_preview_in_use(const uint32_t *buf);
const uint32_t *cam_photo(int slot);
void cam_status_get(struct cam_status *out);

#endif
