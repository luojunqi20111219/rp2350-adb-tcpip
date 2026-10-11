/*
 * usb_host.c - RP2350 PIO-USB 主机初始化
 *
 * 关键点：
 *   1. PIO-USB 挂在 rhport 1（rhport 0 是 RP2350 原生 USB）。
 *   2. 用 tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg) 传配置。
 *   3. 引脚按板子来：RP2350-USB-CM = D+ GP13 / D- GP12（pinout DMDP）。
 */
#include "tusb.h"
#include "pico/stdlib.h"

#include "usb_host.h"
#include "board_config.h"

#if defined(CFG_TUH_RPI_PIO_USB) && CFG_TUH_RPI_PIO_USB
#include "pio_usb.h"
#endif

#define PIO_USB_RHPORT 1

void usb_host_init(void)
{
#if defined(CFG_TUH_RPI_PIO_USB) && CFG_TUH_RPI_PIO_USB
    pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;

    cfg.pin_dp = BOARD_PIO_USB_DP_PIN;
#if BOARD_PIO_USB_DM_IS_DP_MINUS_1
    cfg.pinout = PIO_USB_PINOUT_DMDP; /* D- = D+ - 1  (USB-CM) */
#else
    cfg.pinout = PIO_USB_PINOUT_DPDM; /* D- = D+ + 1  (USB-C)  */
#endif

    tuh_configure(PIO_USB_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
#endif

    tuh_init(PIO_USB_RHPORT);
}

void usb_host_task(void)
{
    tuh_task();
}
