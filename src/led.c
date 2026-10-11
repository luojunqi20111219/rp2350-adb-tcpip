/*
 * led.c - 板载 LED
 *
 * 默认用 WS2812B（GPIO16，PIO2/SM0 驱动）。
 * 如果你的板子不是 WS2812，把 cmake 里的 LED_USE_WS2812 关掉即可退化成普通 GPIO LED。
 */
#include "pico/stdlib.h"

#include "board_config.h"
#include "led.h"

#if LED_USE_WS2812
#include "hardware/pio.h"
#include "ws2812.pio.h"

static PIO  s_pio;
static uint s_sm;
static bool s_inited = false;

void led_init(void)
{
    s_pio = BOARD_LED_PIO;
    s_sm  = BOARD_LED_PIO_SM;

    uint offset = pio_add_program(s_pio, &ws2812_program);
    ws2812_program_init(s_pio, s_sm, offset, BOARD_LED_GPIO, 800000.0f, false);
    s_inited = true;

    led_set(0, 0, 0);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 8) | ((uint32_t)g << 16) | (uint32_t)b;
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    static uint32_t last = 0xFFFFFFFFu;
    uint32_t grb = urgb_u32(r, g, b);

    if (grb == last) return; /* 值没变就不刷，省 PIO FIFO */
    last = grb;

    if (s_inited) {
        pio_sm_put_blocking(s_pio, s_sm, grb << 8u);
    }
}

#else /* ---- 普通 GPIO LED ---- */

static bool s_on_level = true;

void led_init(void)
{
    gpio_init(BOARD_LED_GPIO);
    gpio_set_dir(BOARD_LED_GPIO, GPIO_OUT);
    gpio_put(BOARD_LED_GPIO, !s_on_level);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    bool on = (r | g | b) != 0;
    gpio_put(BOARD_LED_GPIO, on ? s_on_level : !s_on_level);
}

#endif

void led_blink(int times, uint32_t on_ms, uint32_t off_ms,
               uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < times; i++) {
        led_set(r, g, b);
        sleep_ms(on_ms);
        led_set(0, 0, 0);
        sleep_ms(off_ms);
    }
}
