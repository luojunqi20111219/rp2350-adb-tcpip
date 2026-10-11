/*
 * usbh_adb.h - 自定义 TinyUSB 主机类驱动（匹配安卓 adbd 的 vendor 接口）
 *
 * 安卓 adbd 的 USB 接口：bInterfaceClass=0xFF, bInterfaceSubClass=0x42,
 * bInterfaceProtocol=0x01，带一对 Bulk IN/OUT。
 * 现代 TinyUSB 没有内置 vendor host class，所以用 usbh_app_driver_get_cb()
 * 注册自定义类驱动。
 */
#ifndef USBH_ADB_H
#define USBH_ADB_H

#include <stdint.h>
#include <stdbool.h>

/* 是否已枚举到 adb 接口并且端点都开好了 */
bool adb_usb_ready(void);

/* 对 bulk OUT 发一段数据（同步阻塞，内部会 pump tuh_task） */
bool adb_usb_write(const uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/* 从 bulk IN 读最多 len 字节；返回实际字节数，-1 表示失败/超时 */
int adb_usb_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/* 当前设备地址（调试用） */
uint8_t adb_usb_dev_addr(void);

#endif /* USBH_ADB_H */
