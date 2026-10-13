/* N900 speaker playback through the I2S and audio codec APIs */
#include <math.h>
#include <zephyr/audio/codec.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

#define RATE      48000
#define CHANNELS  2
#define FRAMES    960 /* 20 ms */
#define BLOCK     (FRAMES * CHANNELS * sizeof(int16_t))
#define NUM_BLOCKS 6

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(mcbsp2));
static const struct device *const codec = DEVICE_DT_GET(DT_NODELABEL(codec));
static const struct gpio_dt_spec amp =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), speaker_amp_gpios);

K_MEM_SLAB_DEFINE_STATIC(slab, BLOCK, NUM_BLOCKS, 32);

static float phase;

static void fill(int16_t *buf, float freq, float amp_lin)
{
	for (int i = 0; i < FRAMES; i++) {
		int16_t s = (int16_t)(amp_lin * 12000.0f * sinf(phase));

		buf[2 * i] = s;
		buf[2 * i + 1] = s;
		phase += 2.0f * 3.14159265f * freq / RATE;
		if (phase > 2.0f * 3.14159265f) {
			phase -= 2.0f * 3.14159265f;
		}
	}
}

int main(void)
{
	static const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f};
	struct audio_codec_cfg ccfg = {
		.mclk_freq = 19200000,
		.dai_type = AUDIO_DAI_TYPE_PCMA,
		.dai_route = AUDIO_ROUTE_PLAYBACK,
		.dai_cfg.i2s = {
			.word_size = 16,
			.channels = CHANNELS,
			.frame_clk_freq = RATE,
			/* The codec drives both clocks */
			.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		},
	};
	struct i2s_config icfg = {
		.word_size = 16,
		.channels = CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_PCM_SHORT,
		.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET,
		.frame_clk_freq = RATE,
		.mem_slab = &slab,
		.block_size = BLOCK,
		.timeout = 1000,
	};
	audio_property_value_t vol;
	int ret;

	k_msleep(3000);
	printk("audio play: i2s %d codec %d amp %d\n", device_is_ready(i2s),
	       device_is_ready(codec), gpio_is_ready_dt(&amp));

	ret = audio_codec_configure(codec, &ccfg);
	printk("codec configure %d\n", ret);
	ret = i2s_configure(i2s, I2S_DIR_TX, &icfg);
	printk("i2s configure %d\n", ret);
	if (ret != 0) {
		return 0;
	}
	gpio_pin_configure_dt(&amp, GPIO_OUTPUT_ACTIVE);
	audio_codec_start_output(codec);

	for (int rep = 0; rep < 3; rep++) {
		/* Volume steps down 6 dB per repeat: 0, -6, -12 dB */
		vol.vol = -12 * rep;
		audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL,
					 vol);
		audio_codec_apply_properties(codec);

		for (int n = 0; n < (int)ARRAY_SIZE(notes); n++) {
			/* 300 ms per note */
			for (int b = 0; b < 15; b++) {
				void *blk;

				ret = k_mem_slab_alloc(&slab, &blk, K_MSEC(1000));
				if (ret != 0) {
					printk("slab alloc %d\n", ret);
					return 0;
				}
				fill(blk, notes[n], (b < 13) ? 1.0f : 0.0f);
				ret = i2s_write(i2s, blk, BLOCK);
				if (ret != 0) {
					printk("i2s write %d\n", ret);
					return 0;
				}
				if ((rep == 0) && (n == 0) && (b == 2)) {
					ret = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
					printk("start %d\n", ret);
				}
			}
		}
		printk("pass %d done\n", rep);
	}

	ret = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	printk("drain %d\n", ret);
	k_msleep(200);
	audio_codec_stop_output(codec);
	gpio_pin_set_dt(&amp, 0);
	printk("done\n");
	return 0;
}
