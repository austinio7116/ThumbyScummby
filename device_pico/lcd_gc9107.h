/*
 * ThumbyScummby — minimal GC9107 LCD driver for Thumby Color (RP2350).
 *
 * 128x128 RGB565 panel, 4-wire SPI on spi0, 80 MHz.
 * Pin map (board reference):
 *   GP18 = SCK
 *   GP19 = SDA (MOSI)
 *   GP17 = CS  (manual GPIO)
 *   GP16 = DC
 *   GP4  = RST
 *   GP7  = BL  (driven high = full brightness)
 *
 * Lifted verbatim from ThumbyDOOM's doom_lcd_gc9107.c — the init
 * sequence is the documented GC9107 startup register order and is
 * device-mandated.
 */
#ifndef THUMBYSCUMMBY_LCD_GC9107_H
#define THUMBYSCUMMBY_LCD_GC9107_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lcd_init(void);

/* Push a 128x128 RGB565 framebuffer to the panel. Blocks until any
 * previous DMA finishes, then kicks a new DMA transfer. */
void lcd_present(const uint16_t *fb_rgb565);

/* Wait for any in-flight DMA push to complete. Call before reusing
 * the framebuffer for the next frame. */
void lcd_wait_idle(void);

/* Backlight control (GPIO on/off — no PWM dimming). */
void lcd_backlight(int on);

#ifdef __cplusplus
}
#endif

#endif
