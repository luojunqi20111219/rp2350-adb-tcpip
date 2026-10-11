/*
 * tusb_config.h - TinyUSB 裁剪配置（只做 Host，只跑自定义 ADB 类驱动）
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 通用 ---------------- */
#define CFG_TUSB_MCU OPT_MCU_RP2040
#define CFG_TUSB_OS  OPT_OS_PICO

/* Device 栈完全不用（我们只用 PIO-USB 做 Host） */
#define CFG_TUD_ENABLED 0

/* Host 栈 + PIO-USB 后端 */
#define CFG_TUH_ENABLED     1
#define CFG_TUH_RPI_PIO_USB 1

/* ---------------- 内存 ---------------- */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

/* ---------------- Host ---------------- */
/* 枚举缓冲区：安卓 adb gadget 描述符较长，给宽一点 */
#define CFG_TUH_ENUMERATION_BUFSIZE 512

/* 只接一台手机，不要 Hub */
#define CFG_TUH_HUB        0
#define CFG_TUH_DEVICE_MAX 1

/* 关掉所有内置类驱动，全部交给我们的 ADB 自定义驱动 */
#define CFG_TUH_CDC    0
#define CFG_TUH_MSC    0
#define CFG_TUH_HID    0
#define CFG_TUH_MIDI   0
#define CFG_TUH_VENDOR 0

/* 允许 usbh_edpt_xfer 的完成回调路径（自定义类驱动要用） */
#define CFG_TUH_API_EDPT_XFER 1

/* 自定义类驱动数量不占用内置驱动表 */
#define CFG_TUH_LOG_LEVEL 0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
