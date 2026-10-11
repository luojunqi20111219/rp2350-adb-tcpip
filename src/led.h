/*
 * led.h - 板载状态指示
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

void led_init(void);

/* 0..255。建议值 <= 64，别太刺眼。 */
void led_set(uint8_t r, uint8_t g, uint8_t b);

/* 阻塞闪烁：on/off 单位 ms */
void led_blink(int times, uint32_t on_ms, uint32_t off_ms,
               uint8_t r, uint8_t g, uint8_t b);

#endif /* LED_H */
