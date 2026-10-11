/*
 * board_config.h - 板级硬件配置（微雪 RP2350-USB-CM / RP2350-USB-C）
 *
 * 只有这一处需要按实物改动；其余源码与板子无关。
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* ------------------------------------------------------------------ */
/* PIO-USB 引脚                                                        */
/* ------------------------------------------------------------------ */
/*
 * 官方参数（微雪 RP2350-USB-C / -CM 原理图）：
 *
 *   RP2350-USB-CM (Type-C 公头版)  : D+ = GP13, D- = GP12  (pinout = DMDP)
 *   RP2350-USB-C  (Type-C 母座版)  : D+ = GP12, D- = GP13  (pinout = DPDM)
 *
 * 可以在 cmake 里用 -DPIO_USB_DP_PIN=xx 覆盖。
 * 本板"公头"那一路 Type-C 就是 PIO-USB 口，插手机用的正是它。
 */
#ifndef BOARD_PIO_USB_DP_PIN
#define BOARD_PIO_USB_DP_PIN 13
#endif

/* 1 = D- 在 D+ 的“前一号” GPIO (DMDP)，适用于 -CM；0 = DPDM，适用于 -C */
#ifndef BOARD_PIO_USB_DM_IS_DP_MINUS_1
#define BOARD_PIO_USB_DM_IS_DP_MINUS_1 1
#endif

/* ------------------------------------------------------------------ */
/* 板载 LED                                                            */
/* ------------------------------------------------------------------ */
/* 微雪 RP2350-USB-C / -CM 板载 WS2812B RGB LED 数据脚 = GPIO16 */
#ifndef BOARD_LED_GPIO
#define BOARD_LED_GPIO 16
#endif

/* 用哪组 PIO 驱动 WS2812。PIO0/PIO1 已被 PIO-USB 占用，故用 PIO2。 */
#ifndef BOARD_LED_PIO
#define BOARD_LED_PIO pio2
#endif
#ifndef BOARD_LED_PIO_SM
#define BOARD_LED_PIO_SM 0
#endif

/* ------------------------------------------------------------------ */
/* 调试串口                                                            */
/* ------------------------------------------------------------------ */
/* 默认 UART0 (GP0=TX, GP1=RX, 115200)。不需要可置 0 省 flash。 */
#ifndef BOARD_ENABLE_UART_LOG
#define BOARD_ENABLE_UART_LOG 1
#endif

#endif /* BOARD_CONFIG_H */
