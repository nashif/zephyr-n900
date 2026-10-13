/*
 * N900 phone demo: sounds on the loudspeakers. A thread synthesizes tone
 * sequences into I2S blocks for McBSP2; the TLV320AIC34 plays them and the
 * speaker amplifier is on only while the stream runs.
 */
#include <math.h>
#include <string.h>
#include <zephyr/audio/codec.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

#include "sound.h"

#define RATE     48000
#define FRAMES   480 /* 10 ms */
#define BLOCK    (FRAMES * 2 * sizeof(int16_t))
#define BLOCKS   8
#define PRIME    3
/* Fade in and out over 3 ms to avoid clicks */
#define FADE     144
#define IDLE_MS  2000
#define TWO_PI   6.2831853f

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(mcbsp2));
static const struct device *const codec = DEVICE_DT_GET(DT_NODELABEL(codec));
static const struct gpio_dt_spec amp = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), speaker_amp_gpios);

K_MEM_SLAB_DEFINE_STATIC(slab, BLOCK, BLOCKS, 32);

struct note {
	uint16_t f1;
	uint16_t f2;   /* second tone, 0 for none */
	uint16_t ms;
	uint8_t level; /* percent, 0 is a rest */
	bool noise;
};

struct sequence {
	const struct note *notes;
	uint8_t count;
	bool loop;
};

#define N(f, ms)         {(f), 0, (ms), 60, false}
#define REST(ms)         {0, 0, (ms), 0, false}
#define DUAL(a, b, ms)   {(a), (b), (ms), 50, false}

static const struct note click[] = {{1800, 0, 6, 25, false}};
/* An original ringing phrase, then a pause */
static const struct note ringtone[] = {
	N(659, 110), REST(20), N(784, 110), REST(20), N(1047, 110), REST(20), N(784, 110),
	REST(20), N(880, 110), REST(20), N(1175, 220), REST(900),
};
/* European ringback: 425 Hz, one second on, four off */
static const struct note ringback[] = {{425, 0, 1000, 35, false}, REST(4000)};
static const struct note call_end[] = {N(587, 120), REST(40), N(440, 200)};
static const struct note message[] = {N(1047, 90), REST(30), N(1319, 160)};
static const struct note shutter[] = {
	{0, 0, 25, 60, true}, REST(45), {0, 0, 35, 45, true},
};
static const struct note charger[] = {N(784, 80), REST(20), N(1047, 120)};
static const struct note startup[] = {
	N(523, 120), N(659, 120), N(784, 120), N(1047, 360),
};

static const struct sequence sequences[SOUND_COUNT] = {
	[SOUND_CLICK] = {click, ARRAY_SIZE(click), false},
	[SOUND_RINGTONE] = {ringtone, ARRAY_SIZE(ringtone), true},
	[SOUND_RINGBACK] = {ringback, ARRAY_SIZE(ringback), true},
	[SOUND_CALL_END] = {call_end, ARRAY_SIZE(call_end), false},
	[SOUND_MESSAGE] = {message, ARRAY_SIZE(message), false},
	[SOUND_SHUTTER] = {shutter, ARRAY_SIZE(shutter), false},
	[SOUND_CHARGER] = {charger, ARRAY_SIZE(charger), false},
	[SOUND_STARTUP] = {startup, ARRAY_SIZE(startup), false},
};

/* A DTMF key is a sequence of one note */
static struct note dtmf_note;
static struct sequence dtmf_seq = {&dtmf_note, 1, false};

struct request {
	const struct sequence *seq; /* NULL stops */
};

K_MSGQ_DEFINE(sound_q, sizeof(struct request), 8, 4);

/* Player state, sound thread only */
static const struct sequence *cur;
static int note_idx;
static uint32_t note_pos;
static uint32_t note_len;
static float ph1, ph2;
static bool streaming;
static int64_t idle_since;

/* Noise for the shutter; quality does not matter */
static uint32_t lcg = 12345U;

static float noise(void)
{
	lcg = lcg * 1664525U + 1013904223U;
	return ((float)(lcg >> 16) / 32768.0f) - 1.0f;
}

static void note_start(void)
{
	note_pos = 0U;
	note_len = (uint32_t)cur->notes[note_idx].ms * RATE / 1000U;
}

static int16_t sample(const struct note *n)
{
	float env = 1.0f;
	float v;

	if (n->level == 0U) {
		return 0;
	}
	if (note_pos < FADE) {
		env = (float)note_pos / FADE;
	} else if (note_len - note_pos < FADE) {
		env = (float)(note_len - note_pos) / FADE;
	}
	if (n->noise) {
		v = noise();
	} else {
		v = sinf(ph1);
		ph1 += TWO_PI * n->f1 / RATE;
		if (ph1 > TWO_PI) {
			ph1 -= TWO_PI;
		}
		if (n->f2 != 0U) {
			v = 0.5f * (v + sinf(ph2));
			ph2 += TWO_PI * n->f2 / RATE;
			if (ph2 > TWO_PI) {
				ph2 -= TWO_PI;
			}
		}
	}
	return (int16_t)(v * env * n->level * 300.0f);
}

/* Fills a block from the current sequence, silence once it has ended */
static void fill(int16_t *buf)
{
	for (int i = 0; i < FRAMES; i++) {
		int16_t s = 0;

		if (cur != NULL) {
			s = sample(&cur->notes[note_idx]);
			if (++note_pos >= note_len) {
				if (++note_idx >= cur->count) {
					note_idx = 0;
					if (!cur->loop) {
						cur = NULL;
					}
				}
				if (cur != NULL) {
					note_start();
				}
			}
		}
		buf[2 * i] = s;
		buf[2 * i + 1] = s;
	}
}

static int write_block(void)
{
	void *blk;
	int ret;

	ret = k_mem_slab_alloc(&slab, &blk, K_MSEC(200));
	if (ret != 0) {
		return ret;
	}
	fill(blk);
	ret = i2s_write(i2s, blk, BLOCK);
	if (ret != 0) {
		k_mem_slab_free(&slab, blk);
	}
	return ret;
}

static void stream_start(void)
{
	int ret;

	/* The previous stream may still be draining */
	for (int i = 0; i < 20; i++) {
		ret = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DROP);
		if (ret == 0) {
			break;
		}
		k_msleep(5);
	}
	gpio_pin_set_dt(&amp, 1);
	for (int i = 0; i < PRIME; i++) {
		(void)write_block();
	}
	if (i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START) == 0) {
		streaming = true;
	}
}

static void stream_stop(void)
{
	(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	streaming = false;
	/* Let the last blocks play out before the amplifier goes off */
	k_msleep(BLOCKS * 10 + 30);
	gpio_pin_set_dt(&amp, 0);
}

static void sound_thread(void *a, void *b, void *c)
{
	struct audio_codec_cfg ccfg = {
		.mclk_freq = 19200000,
		.dai_type = AUDIO_DAI_TYPE_PCMA,
		.dai_route = AUDIO_ROUTE_PLAYBACK,
		.dai_cfg.i2s = {
			.word_size = 16,
			.channels = 2,
			.frame_clk_freq = RATE,
			.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		},
	};
	struct i2s_config icfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_PCM_SHORT,
		.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET,
		.frame_clk_freq = RATE,
		.mem_slab = &slab,
		.block_size = BLOCK,
		.timeout = 200,
	};

	if (!device_is_ready(i2s) || !device_is_ready(codec) || !gpio_is_ready_dt(&amp) ||
	    (audio_codec_configure(codec, &ccfg) != 0) ||
	    (i2s_configure(i2s, I2S_DIR_TX, &icfg) != 0)) {
		printk("sound: not available\n");
		return;
	}
	gpio_pin_configure_dt(&amp, GPIO_OUTPUT_INACTIVE);
	audio_codec_start_output(codec);

	for (;;) {
		struct request req;
		k_timeout_t wait = streaming ? K_NO_WAIT : K_FOREVER;

		while (k_msgq_get(&sound_q, &req, wait) == 0) {
			wait = K_NO_WAIT;
			cur = req.seq;
			note_idx = 0;
			ph1 = 0.0f;
			ph2 = 0.0f;
			if (cur != NULL) {
				note_start();
				idle_since = 0;
				if (!streaming) {
					stream_start();
				}
			}
		}
		if (!streaming) {
			continue;
		}

		if (write_block() == -EIO) {
			/* Underrun: start over */
			(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
			streaming = false;
			gpio_pin_set_dt(&amp, 0);
			continue;
		}

		if (cur == NULL) {
			if (idle_since == 0) {
				idle_since = k_uptime_get();
			} else if (k_uptime_get() - idle_since > IDLE_MS) {
				stream_stop();
			}
		}
	}
}

K_THREAD_STACK_DEFINE(sound_stack, 2048);
static struct k_thread sound_tid;

void sound_init(void)
{
	k_thread_create(&sound_tid, sound_stack, K_THREAD_STACK_SIZEOF(sound_stack), sound_thread,
			NULL, NULL, NULL, 5, 0, K_NO_WAIT);
}

static void post(const struct sequence *seq)
{
	struct request req = {.seq = seq};

	(void)k_msgq_put(&sound_q, &req, K_NO_WAIT);
}

void sound_play(enum sound_id id)
{
	if (id < SOUND_COUNT) {
		post(&sequences[id]);
	}
}

void sound_dtmf(char key)
{
	static const uint16_t rows[] = {697, 770, 852, 941};
	static const uint16_t cols[] = {1209, 1336, 1477};
	static const char keys[] = "123456789*0#";
	const char *p = strchr(keys, key);
	int i;

	if ((p == NULL) || (key == '\0')) {
		return;
	}
	i = p - keys;
	/* The sound thread reads the note once the request arrives */
	dtmf_note = (struct note)DUAL(rows[i / 3], cols[i % 3], 140);
	post(&dtmf_seq);
}

void sound_stop(void)
{
	post(NULL);
}

void sound_set_volume(int pct)
{
	audio_property_value_t v;

	pct = CLAMP(pct, 0, 100);
	v.mute = (pct == 0);
	audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL, v);
	/* 0.5 dB steps: 100 % is 0 dB, 1 % is -40 dB */
	v.vol = -((100 - pct) * 80 / 100);
	audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, v);
	audio_codec_apply_properties(codec);
}
