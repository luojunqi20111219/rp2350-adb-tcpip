/*
 * main.c - RP2350-USB-CM 脱机 ADB 客户端
 *
 * 上电 -> PIO-USB 做 Host -> 插手机 -> 自动 ADB 握手
 *      -> adb tcpip 5555 -> (adbd 重启, USB 重新枚举, 自动重新握手)
 *      -> adb shell sh .../shizuku.../start.sh
 *      -> LED 提示，全程不需要电脑。
 */
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/time.h"

#include "board_config.h"
#include "usb_host.h"
#include "usbh_adb.h"
#include "adb_client.h"
#include "led.h"

/* 状态灯颜色（不用太亮） */
#define C_WAIT_R 0
#define C_WAIT_G 0
#define C_WAIT_B 24

int main(void)
{
#if BOARD_ENABLE_UART_LOG
    stdio_uart_init(); /* UART0: GP0=TX, GP1=RX, 115200 */
#endif

    led_init();
    led_set(C_WAIT_R, C_WAIT_G, C_WAIT_B);

    printf("\n=== RP2350-USB-CM offline ADB client ===\n");
    printf("PIO-USB D+ = GP%d, LED = GP%d\n", BOARD_PIO_USB_DP_PIN, BOARD_LED_GPIO);
    printf("把 Type-C 公头插进手机（手机需已开启“USB 调试”）...\n");

    usb_host_init();

    bool busy = false; /* 本机已挂载并且流程已跑过 */
    bool was_ready = false;

    while (true) {
        usb_host_task();

        bool ready = adb_usb_ready();

        if (ready && !was_ready) {
            printf("[usb] adb device mounted, addr=%u\n", adb_usb_dev_addr());
            led_set(0, 24, 0); /* 绿：已识别 */
        } else if (!ready && was_ready) {
            printf("[usb] adb device unmounted\n");
        }
        was_ready = ready;

        if (ready && !busy) {
            busy = true;

            adb_result_t r = adb_client_run();

            led_set(0, 0, 0);
            if (r == ADB_RESULT_OK) {
                led_blink(3, 60, 60, 0, 64, 0); /* 成功：绿快闪 3 次 */
            } else {
                printf("[adb] FAILED: %s\n", adb_client_last_error());
                led_blink(4, 400, 400, 64, 0, 0); /* 失败：红慢闪 */
            }
            led_set(0, 0, 0);
            /* 到此为止，不再动作；拔插后可重来 */
        }

        if (!ready) {
            busy = false;
            led_set(C_WAIT_R, C_WAIT_G, C_WAIT_B);
        }

        sleep_ms(1);
    }
}
