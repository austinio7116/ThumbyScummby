/*
 * ThumbyScummby — RP2350 PWM audio driver implementation.
 * Lifted from ThumbyDOOM with sample rate adjusted for our engine.
 */
#include "audio_pwm.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#define AUDIO_PWM_PIN     23
#define AUDIO_ENABLE_PIN  20
#define TIMER_SLICE        4
#define SAMPLE_RATE     22050   /* matches engine audio_mix rate */
#define PWM_WRAP          4096  /* 12-bit DAC */

/* THUMBY-PORT: 2048 samples × 2 bytes = 4 KB BSS, ~93 ms latency at
 * 22050 Hz.  Halved from 4096 to free RAM for ScummVM's heap; a frame
 * stall longer than 93 ms would underrun audio (rare in steady-state
 * play, possible during scene loads). */
#define RING_SIZE 2048
#define RING_MASK (RING_SIZE - 1)

static volatile int16_t ring[RING_SIZE];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;

static void __isr __not_in_flash_func(audio_irq) (void) {
    pwm_clear_irq(TIMER_SLICE);

    int16_t s = 0;
    uint32_t t = ring_tail;
    if (t != ring_head) {
        s = ring[t & RING_MASK];
        ring_tail = t + 1;
    }
    /* Triangular dither for cleaner quantization noise floor. */
    static uint32_t dither_rng = 1;
    dither_rng = dither_rng * 1103515245u + 12345u;
    int d1 = (int)(dither_rng >> 20) & 0x1F;
    dither_rng = dither_rng * 1103515245u + 12345u;
    int d2 = (int)(dither_rng >> 20) & 0x1F;
    int dither = d1 + d2 - 31;

    /* int16 [-32768..32767] -> uint [0..PWM_WRAP-1] */
    int v = ((int)s + 32768 + dither) >> 4;
    if (v < 0) v = 0;
    if (v >= PWM_WRAP) v = PWM_WRAP - 1;
    pwm_set_gpio_level(AUDIO_PWM_PIN, (uint32_t)v);
}

void audio_pwm_init(void) {
    gpio_init(AUDIO_ENABLE_PIN);
    gpio_set_dir(AUDIO_ENABLE_PIN, GPIO_OUT);
    gpio_put(AUDIO_ENABLE_PIN, 0);

    gpio_set_function(AUDIO_PWM_PIN, GPIO_FUNC_PWM);
    uint pwm_slice = pwm_gpio_to_slice_num(AUDIO_PWM_PIN);
    pwm_config audio_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&audio_cfg, 1);
    pwm_config_set_wrap(&audio_cfg, PWM_WRAP);
    pwm_init(pwm_slice, &audio_cfg, true);
    pwm_set_gpio_level(AUDIO_PWM_PIN, PWM_WRAP / 2);

    /* Register the IRQ handler now but DO NOT enable it yet — that's
     * deferred to audio_pwm_irq_enable() so the engine's lengthy init
     * isn't continuously interrupted at 22050 Hz. The engine
     * (platform::audio_init) calls the enable hook once it's ready
     * to take samples. */
    pwm_clear_irq(TIMER_SLICE);
    pwm_set_irq_enabled(TIMER_SLICE, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_irq);
    irq_set_priority(PWM_IRQ_WRAP, PICO_LOWEST_IRQ_PRIORITY);
    /* irq_set_enabled deferred — see audio_pwm_irq_enable */

    pwm_config timer_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&timer_cfg, 1);
    pwm_config_set_wrap(&timer_cfg,
        (uint16_t)((clock_get_hz(clk_sys) / SAMPLE_RATE) - 1));
    pwm_init(TIMER_SLICE, &timer_cfg, true);

    gpio_put(AUDIO_ENABLE_PIN, 1);
}

void audio_pwm_irq_enable(void) {
    irq_set_enabled(PWM_IRQ_WRAP, true);
}

void audio_pwm_push(const int16_t *samples, int n_samples) {
    for (int i = 0; i < n_samples; i++) {
        uint32_t h = ring_head;
        if ((h - ring_tail) >= RING_SIZE) break;
        ring[h & RING_MASK] = samples[i];
        ring_head = h + 1;
    }
}

int audio_pwm_room(void) {
    return RING_SIZE - (int)(ring_head - ring_tail);
}
