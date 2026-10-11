/*
 * usb_host.h - PIO-USB Host 初始化
 */
#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdbool.h>

/* 初始化 PIO-USB 主机（内部完成 tuh_configure + tuh_init） */
void usb_host_init(void);

/* 主循环里定期调用（转发 tuh_task） */
void usb_host_task(void);

#endif /* USB_HOST_H */
