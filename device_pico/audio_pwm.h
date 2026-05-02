/*
 * ThumbyScummby — RP2350 PWM audio driver.
 *
 * Hardware path:
 *   GP23 = PWM audio output (12-bit, ~488 kHz carrier at 250 MHz clk_sys)
 *   GP20 = audio amplifier enable
 *   PWM slice 4 = sample-rate timer (22050 Hz IRQ)
 *
 * Lifted from ThumbyDOOM. Sample rate trimmed from 49716 -> 22050 Hz
 * to match the ThumbyScummby engine's audio_mix_callback rate.
 */
#ifndef THUMBYSCUMMBY_AUDIO_PWM_H
#define THUMBYSCUMMBY_AUDIO_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_pwm_init(void);

/* Push n_samples of int16 mono PCM into the ring. Drops samples if
 * the ring is full. */
void audio_pwm_push(const int16_t *samples, int n_samples);

/* Free room in the ring (in samples). */
int  audio_pwm_room(void);

#ifdef __cplusplus
}
#endif

#endif
