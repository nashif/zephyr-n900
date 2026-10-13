/* N900 phone demo: speaker sounds */
#ifndef SOUND_H_
#define SOUND_H_

enum sound_id {
	SOUND_CLICK,
	SOUND_RINGTONE,  /* repeats until sound_stop() */
	SOUND_RINGBACK,  /* repeats until sound_stop() */
	SOUND_CALL_END,
	SOUND_MESSAGE,
	SOUND_SHUTTER,
	SOUND_CHARGER,
	SOUND_STARTUP,
	SOUND_COUNT,
};

void sound_init(void);
/* Replaces whatever is playing */
void sound_play(enum sound_id id);
/* DTMF tone of a dial pad key: 0-9, '*' or '#' */
void sound_dtmf(char key);
void sound_stop(void);
/* 0 mutes, 100 is full scale */
void sound_set_volume(int pct);

#endif
