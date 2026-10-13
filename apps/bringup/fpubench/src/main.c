/* Floating-point speed with and without the VFP/NEON unit */
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define N 200000

static volatile float vf = 1.0001f;
static volatile double vd = 1.0000001;
static float buf_a[1024], buf_b[1024], buf_c[1024];

static uint32_t us_since(uint32_t start)
{
	return k_cyc_to_us_floor32(k_cycle_get_32() - start);
}

int main(void)
{
	uint32_t t;
	float f = 0.0f;
	double d = 0.0;

	uint32_t sctlr, actlr, c0, c1;

	printk("fpu bench, FPU %s\n", IS_ENABLED(CONFIG_FPU) ? "on" : "off");
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
	printk("SCTLR %08x ACTLR %08x (L2EN %u) PLL_MPU %08x %08x\n", sctlr, actlr,
	       (actlr >> 1) & 1U, sys_read32(0x48004940), sys_read32(0x48004944));
	/* PMU cycle counter against the system timer */
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(1U | 4U));
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(BIT(31)));
	t = k_cycle_get_32();
	__asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c0));
	k_busy_wait(100000);
	__asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c1));
	printk("CPU clock %u MHz\n", (c1 - c0) / us_since(t));
	k_msleep(500);

	for (int i = 0; i < 1024; i++) {
		buf_a[i] = (float)i * 0.5f;
		buf_b[i] = (float)(1024 - i) * 0.25f;
	}

	t = k_cycle_get_32();
	for (int i = 0; i < N; i++) {
		f = f * vf + 0.5f;
	}
	printk("float mul-add x%d: %u us (%d)\n", N, us_since(t), (int)f);

	t = k_cycle_get_32();
	for (int i = 0; i < N; i++) {
		d = d * vd + 0.5;
	}
	printk("double mul-add x%d: %u us (%d)\n", N, us_since(t), (int)d);

	t = k_cycle_get_32();
	f = 0.0f;
	for (int i = 0; i < N / 10; i++) {
		f += sinf((float)i * 0.001f);
	}
	printk("sinf x%d: %u us (%d)\n", N / 10, us_since(t), (int)f);

	t = k_cycle_get_32();
	d = 0.0;
	for (int i = 0; i < N / 10; i++) {
		d += sqrt((double)i);
	}
	printk("sqrt x%d: %u us (%d)\n", N / 10, us_since(t), (int)d);

	memset(buf_c, 0, sizeof(buf_c));
	t = k_cycle_get_32();
	for (int r = 0; r < 200; r++) {
		for (int i = 0; i < 1024; i++) {
			buf_c[i] = buf_a[i] * buf_b[i] + buf_c[i] * 0.5f;
		}
	}
	printk("float vector 200x1024: %u us (%d)\n", us_since(t), (int)buf_c[100]);
#ifdef __ARM_NEON
	memset(buf_c, 0, sizeof(buf_c));
	t = k_cycle_get_32();
	for (int r = 0; r < 200; r++) {
		for (int i = 0; i < 1024; i += 4) {
			float32x4_t a = vld1q_f32(&buf_a[i]);
			float32x4_t b = vld1q_f32(&buf_b[i]);
			float32x4_t c = vld1q_f32(&buf_c[i]);

			vst1q_f32(&buf_c[i], vmlaq_f32(vmulq_n_f32(c, 0.5f), a, b));
		}
	}
	printk("NEON vector 200x1024: %u us (%d)\n", us_since(t), (int)buf_c[100]);
#endif
	printk("printf %%f: %f\n", 3.14159);

	for (int i = 0;; i++) {
		k_msleep(1000);
		printk("tick %d\n", i);
	}
	return 0;
}
