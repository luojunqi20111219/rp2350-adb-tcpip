/*
 * usbh_adb.c - 最小 ADB 主机类驱动
 *
 * open(): 认接口 (0xFF/0x42/0x01) → 开两个 bulk 端点 → 返回消耗的描述符长度
 * set_config(): 标记可用，并调用 usbh_driver_set_config_complete() 推进枚举
 * xfer_cb(): 收传输结果
 * close(): 掉线清理
 */
#include <string.h>

#include "tusb.h"
#include "host/usbh_pvt.h"
#include "pico/time.h"

#include "usbh_adb.h"
#include "board_config.h"

#define ADB_ITF_CLASS    0xFF
#define ADB_ITF_SUBCLASS 0x42
#define ADB_ITF_PROTOCOL 0x01

typedef struct {
    bool     claimed;
    bool     mounted;
    uint8_t  dev_addr;
    uint8_t  itf_num;
    uint8_t  ep_in;
    uint8_t  ep_out;
    uint16_t ep_in_size;
    uint16_t ep_out_size;
} adb_usb_dev_t;

static adb_usb_dev_t s_dev;

/* 传输完成信息：index 0 = OUT，1 = IN */
static volatile xfer_result_t s_result[2];
static volatile uint32_t      s_xfer_len[2];
static volatile bool          s_done[2];

/* ---------------------------------------------------------------- */
/* 同步等待：内部 pump tuh_task()（单线程，重入安全，参考官方示例）   */
/* ---------------------------------------------------------------- */
static bool ep_wait(uint8_t dev_addr, uint8_t ep, uint32_t timeout_ms)
{
    uint8_t dir = (ep & 0x80u) ? 1u : 0u;
    absolute_time_t t0 = get_absolute_time();

    while (usbh_edpt_busy(dev_addr, ep)) {
        tuh_task();
        if (timeout_ms != 0 &&
            absolute_time_diff_us(t0, get_absolute_time()) > (int64_t)timeout_ms * 1000) {
            usbh_edpt_release(dev_addr, ep);
            return false;
        }
    }
    return s_done[dir] && s_result[dir] == XFER_RESULT_SUCCESS;
}

static bool ep_transfer(uint8_t dev_addr, uint8_t ep, const uint8_t *buf,
                        uint16_t len, uint32_t timeout_ms)
{
    uint8_t dir = (ep & 0x80u) ? 1u : 0u;

    s_done[dir] = false;
    s_result[dir] = XFER_RESULT_INVALID;

    if (!usbh_edpt_claim(dev_addr, ep)) return false;
    if (!usbh_edpt_xfer(dev_addr, ep, (uint8_t *)buf, len)) {
        usbh_edpt_release(dev_addr, ep);
        return false;
    }
    return ep_wait(dev_addr, ep, timeout_ms);
}

/* ---------------------------------------------------------------- */
/* 对外 API                                                          */
/* ---------------------------------------------------------------- */
bool adb_usb_ready(void)
{
    return s_dev.claimed && s_dev.mounted && s_dev.ep_in && s_dev.ep_out;
}

uint8_t adb_usb_dev_addr(void) { return s_dev.dev_addr; }

bool adb_usb_write(const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (!adb_usb_ready() || len == 0) return false;

    uint8_t daddr = s_dev.dev_addr;
    uint8_t ep    = s_dev.ep_out;
    uint16_t chunk = s_dev.ep_out_size ? s_dev.ep_out_size : 64;

    while (len > 0) {
        uint16_t n = (len > chunk) ? chunk : len;
        if (!ep_transfer(daddr, ep, buf, n, timeout_ms)) return false;
        buf += n;
        len = (uint16_t)(len - n);
    }
    return true;
}

int adb_usb_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (!adb_usb_ready() || len == 0) return -1;

    if (!ep_transfer(s_dev.dev_addr, s_dev.ep_in, buf, len, timeout_ms)) return -1;
    return (int)s_xfer_len[1];
}

/* ---------------------------------------------------------------- */
/* TinyUSB 主机类驱动                                                */
/* ---------------------------------------------------------------- */
static bool adb_init(void)
{
    memset(&s_dev, 0, sizeof(s_dev));
    s_done[0] = s_done[1] = false;
    return true;
}

static bool adb_deinit(void) { return true; }

static uint16_t adb_open(uint8_t rhport, uint8_t dev_addr,
                         const tusb_desc_interface_t *itf_desc, uint16_t max_len)
{
    (void)rhport;

    if (itf_desc->bInterfaceClass    != ADB_ITF_CLASS ||
        itf_desc->bInterfaceSubClass != ADB_ITF_SUBCLASS ||
        itf_desc->bInterfaceProtocol != ADB_ITF_PROTOCOL) {
        return 0; /* 不是 ADB 接口 */
    }

    s_dev.dev_addr   = dev_addr;
    s_dev.ep_in      = 0;
    s_dev.ep_out     = 0;
    s_dev.ep_in_size = 0;
    s_dev.ep_out_size = 0;

    uint16_t drv_len = (uint16_t)sizeof(tusb_desc_interface_t);
    const uint8_t *p_desc = (const uint8_t *)itf_desc;
    uint8_t found = 0;

    while (found < itf_desc->bNumEndpoints && drv_len < max_len) {
        p_desc = tu_desc_next(p_desc);

        if (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT) {
            drv_len = (uint16_t)(drv_len + tu_desc_len(p_desc));
            continue;
        }

        const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p_desc;

        if (ep->bmAttributes.xfer == TUSB_XFER_BULK) {
            if (!tuh_edpt_open(dev_addr, ep)) return 0;

            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                s_dev.ep_in      = ep->bEndpointAddress;
                s_dev.ep_in_size = (uint16_t)tu_edpt_packet_size(ep);
            } else {
                s_dev.ep_out      = ep->bEndpointAddress;
                s_dev.ep_out_size = (uint16_t)tu_edpt_packet_size(ep);
            }
        }

        drv_len = (uint16_t)(drv_len + tu_desc_len(p_desc));
        found++;
    }

    if (!s_dev.ep_in || !s_dev.ep_out) {
        s_dev.ep_in = s_dev.ep_out = 0;
        return 0;
    }

    s_dev.itf_num = itf_desc->bInterfaceNumber;
    s_dev.claimed = true;
    return drv_len;
}

static bool adb_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    if (s_dev.claimed && s_dev.dev_addr == dev_addr) {
        s_dev.mounted = true;
    }
    /* 必须调用它推进枚举链，否则不会触发 tuh_mount_cb */
    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool adb_xfer_cb(uint8_t dev_addr, uint8_t ep_addr,
                        xfer_result_t result, uint32_t xferred_bytes)
{
    (void)dev_addr;
    uint8_t dir = (ep_addr & 0x80u) ? 1u : 0u;
    s_result[dir]   = result;
    s_xfer_len[dir] = xferred_bytes;
    s_done[dir]     = true;
    return true;
}

static void adb_close(uint8_t dev_addr)
{
    (void)dev_addr;
    memset(&s_dev, 0, sizeof(s_dev));
}

/* 驱动实例 */
static const usbh_class_driver_t adb_driver = {
    .name       = "ADB",
    .init       = adb_init,
    .deinit     = adb_deinit,
    .open       = adb_open,
    .set_config = adb_set_config,
    .xfer_cb    = adb_xfer_cb,
    .close      = adb_close,
};

/* 应用驱动优先级最高，会被优先匹配 */
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &adb_driver;
}
