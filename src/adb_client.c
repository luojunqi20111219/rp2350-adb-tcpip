/*
 * adb_client.c - 最小 ADB 协议栈
 *
 * 流程：
 *   ① CNXN                       发起连接
 *   ② AUTH(TOKEN) → AUTH(SIGN)   用内置私钥签名 challenge
 *     失败则 AUTH(RSAPUBLICKEY)   手机上点“始终允许”
 *   ③ CNXN                       认证通过
 *   ④ adb tcpip 5555             OPEN("tcpip:5555")
 *        └ adbd 会重启并重新枚举 USB
 *          → 固件自动等 USB 掉线 + 等重新枚举 + 重新握手
 *   ⑤ adb shell sh .../start.sh  OPEN("shell:sh /sdcard/...")
 *   ⑥ 服务正常关闭                → 成功
 */
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/time.h"

#include "adb_proto.h"
#include "adb_client.h"
#include "adb_auth.h"
#include "adb_config.h"
#include "usbh_adb.h"
#include "usb_host.h"

#define ADB_LOCAL_ID 1u

static adb_packet_t s_pkt;
static const char  *s_err = "";

const char *adb_client_last_error(void) { return s_err; }

/* ---------------------------------------------------------------- */
/* CRC-32 (IEEE 802.3, 与 zlib/AOSP adb_crc32 一致)                  */
/* ---------------------------------------------------------------- */
static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/* ---------------------------------------------------------------- */
/* 收 / 发                                                            */
/* ---------------------------------------------------------------- */
static bool adb_send(uint32_t cmd, uint32_t arg0, uint32_t arg1,
                     const void *payload, uint32_t len)
{
    if (len > ADB_MAXDATA) { s_err = "tx too big"; return false; }

    s_pkt.msg.command     = cmd;
    s_pkt.msg.arg0        = arg0;
    s_pkt.msg.arg1        = arg1;
    s_pkt.msg.data_length = len;
    s_pkt.msg.data_crc32  = (len != 0) ? crc32_bytes((const uint8_t *)payload, len) : 0;
    s_pkt.msg.magic       = ADB_MAGIC(cmd);

    if (!adb_usb_write((const uint8_t *)&s_pkt.msg, (uint16_t)sizeof(adb_msg_t), ADB_SERVICE_MS)) {
        s_err = "tx header";
        return false;
    }
    if (len != 0) {
        if (!adb_usb_write((const uint8_t *)payload, (uint16_t)len, ADB_SERVICE_MS)) {
            s_err = "tx payload";
            return false;
        }
    }
    return true;
}

static bool adb_recv(uint32_t timeout_ms)
{
    int n = adb_usb_read((uint8_t *)&s_pkt.msg, (uint16_t)sizeof(adb_msg_t), timeout_ms);
    if (n != (int)sizeof(adb_msg_t)) { s_err = "rx header"; return false; }

    if (s_pkt.msg.command != (s_pkt.msg.magic ^ 0xFFFFFFFFu)) { s_err = "bad magic"; return false; }
    if (s_pkt.msg.data_length > ADB_MAXDATA)                   { s_err = "rx too big"; return false; }

    uint32_t got = 0;
    while (got < s_pkt.msg.data_length) {
        int m = adb_usb_read(s_pkt.data + got,
                             (uint16_t)(s_pkt.msg.data_length - got), timeout_ms);
        if (m <= 0) { s_err = "rx payload"; return false; }
        got += (uint32_t)m;
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* USB 状态轮询（内部 pump 主机栈）                                   */
/* ---------------------------------------------------------------- */
static bool wait_usb(bool want_ready, uint32_t timeout_ms)
{
    absolute_time_t t0 = get_absolute_time();

    while (adb_usb_ready() != want_ready) {
        usb_host_task();
        if (absolute_time_diff_us(t0, get_absolute_time()) > (int64_t)timeout_ms * 1000) {
            return false;
        }
        sleep_ms(1);
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* 握手：CNXN + AUTH                                                 */
/* ---------------------------------------------------------------- */
static bool send_cnxn(void)
{
    static const char ID[]     = "host::";
    static const char BANNER[] = "features=cmd,stat_v2,ls_v2";
    char payload[128];
    size_t len = 0;

    memcpy(&payload[len], ID, sizeof(ID));         len += sizeof(ID);
    memcpy(&payload[len], BANNER, sizeof(BANNER)); len += sizeof(BANNER);

    return adb_send(ADB_CMD_CNXN, ADB_VERSION, ADB_MAXDATA, payload, (uint32_t)len);
}

static bool send_auth_signature(void)
{
    uint8_t sig[256];
    if (!adb_auth_sign(s_pkt.data, s_pkt.msg.data_length, sig)) {
        s_err = "rsa sign";
        return false;
    }
    return adb_send(ADB_CMD_AUTH, ADB_AUTH_SIGNATURE, 0, sig, sizeof(sig));
}

static bool send_auth_pubkey(void)
{
    const char *pk = adb_auth_public_key_string();
    uint32_t len = (uint32_t)strlen(pk) + 1u;
    return adb_send(ADB_CMD_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, pk, len);
}

static bool adb_handshake(void)
{
    printf("[adb] CNXN ...\n");
    if (!send_cnxn()) return false;

    bool sig_sent    = false;
    bool pubkey_sent = false;

    for (int guard = 0; guard < 8; guard++) {
        uint32_t to = pubkey_sent ? ADB_PUBKEY_MS : ADB_AUTH_MS;
        if (!adb_recv(to)) return false;

        if (s_pkt.msg.command == ADB_CMD_CNXN) {
            printf("[adb] CNXN ok (version=%08x)\n", s_pkt.msg.arg0);
            return true;
        }
        if (s_pkt.msg.command == ADB_CMD_AUTH && s_pkt.msg.arg0 == ADB_AUTH_TOKEN) {
            if (!sig_sent) {
                printf("[adb] AUTH token -> RSA-2048 / SHA1-DigestInfo 签名\n");
                if (!send_auth_signature()) return false;
                sig_sent = true;
            } else if (!pubkey_sent) {
                printf("[adb] 签名被拒 -> 发送公钥，手机上请勾“始终允许”\n");
                if (!send_auth_pubkey()) return false;
                pubkey_sent = true;
            } else {
                s_err = "auth rejected";
                return false;
            }
            continue;
        }
        s_err = "unexpected pkt in auth";
        return false;
    }
    s_err = "auth loop";
    return false;
}

/* ---------------------------------------------------------------- */
/* 打开一个服务通道                                                   */
/*   service      服务名，如 "tcpip:5555" / "shell:sh /sdcard/..."     */
/*   out/out_cap  收集输出（可为 NULL）                               */
/*   stop_at_okay 收到 OKAY 就算完成。tcpip 用它 —— 因为它会让 adbd    */
/*                重启，后面不会再有 CLSE                              */
/* ---------------------------------------------------------------- */
static adb_result_t run_service(const char *service, char *out, size_t out_cap,
                                bool stop_at_okay, uint32_t idle_ms)
{
    if (!adb_send(ADB_CMD_OPEN, ADB_LOCAL_ID, 0, service, (uint32_t)strlen(service) + 1u)) {
        s_err = "OPEN tx";
        return ADB_RESULT_FAIL;
    }

    uint32_t remote_id = 0;
    bool     opened    = false;
    size_t   out_len   = 0;
    if (out && out_cap) out[0] = '\0';

    for (int guard = 0; guard < 256; guard++) {
        if (!adb_recv(idle_ms)) return ADB_RESULT_FAIL;
        if (s_pkt.msg.arg1 != ADB_LOCAL_ID) continue; /* 不是我们这条流 */

        switch (s_pkt.msg.command) {
        case ADB_CMD_OKAY:
            remote_id = s_pkt.msg.arg0;
            opened    = true;
            if (stop_at_okay) {
                adb_send(ADB_CMD_CLSE, ADB_LOCAL_ID, remote_id, NULL, 0);
                return ADB_RESULT_OK;
            }
            break;

        case ADB_CMD_WRTE:
            opened = true;
            if (out && out_cap && s_pkt.msg.data_length != 0 &&
                out_len + s_pkt.msg.data_length < out_cap) {
                memcpy(out + out_len, s_pkt.data, s_pkt.msg.data_length);
                out_len += s_pkt.msg.data_length;
                out[out_len] = '\0';
            }
            /* 不回 OKAY 设备就不会继续发下一段 */
            adb_send(ADB_CMD_OKAY, ADB_LOCAL_ID, s_pkt.msg.arg0, NULL, 0);
            break;

        case ADB_CMD_CLSE:
            if (s_pkt.msg.arg0 != 0) {
                adb_send(ADB_CMD_CLSE, ADB_LOCAL_ID, s_pkt.msg.arg0, NULL, 0);
            }
            if (!opened) { s_err = "service refused"; return ADB_RESULT_FAIL; }
            return ADB_RESULT_OK;

        default:
            break;
        }
    }
    s_err = "service loop";
    return ADB_RESULT_FAIL;
}

/* ---------------------------------------------------------------- */
/* ① adb tcpip <port>                                                */
/* ---------------------------------------------------------------- */
static adb_result_t do_tcpip(void)
{
    char svc[64];
    snprintf(svc, sizeof(svc), "tcpip:%s", ADB_CMD_TCPIP_PORT);

    printf("[adb] OPEN %s\n", svc);
    if (run_service(svc, NULL, 0, true, ADB_SERVICE_MS) != ADB_RESULT_OK) {
        return ADB_RESULT_FAIL;
    }
    printf("[adb] tcpip 已生效\n");

    /* adbd 会重启 -> USB 断开 -> 重新枚举 */
    printf("[adb] 等待 adbd 重启 / USB 重新枚举 ...\n");
    if (wait_usb(false, ADB_UNPLUG_MS)) {
        if (!wait_usb(true, ADB_REENUM_MS)) { s_err = "reenum timeout"; return ADB_RESULT_FAIL; }
        printf("[adb] USB 已回来，重新握手\n");
        sleep_ms(ADB_RETRY_DELAY_MS);
        if (!adb_handshake()) return ADB_RESULT_FAIL;
    } else {
        printf("[adb] USB 未断开（该机型未重启 adbd），沿用当前会话\n");
    }
    return ADB_RESULT_OK;
}

/* ---------------------------------------------------------------- */
/* ② adb shell <cmd>                                                 */
/* ---------------------------------------------------------------- */
static adb_result_t do_shell(void)
{
    char svc[192];
    char out[ADB_SHELL_OUT_MAX];

    snprintf(svc, sizeof(svc), "shell:%s", ADB_CMD_SHELL);
    printf("[adb] OPEN %s\n", svc);

    out[0] = '\0';
    adb_result_t r = run_service(svc, out, sizeof(out), false, ADB_SHELL_MS);
    if (r == ADB_RESULT_OK) {
        if (out[0]) printf("[adb] shell 输出:\n%s\n", out);
        else        printf("[adb] shell 无输出\n");
    }
    return r;
}

/* ---------------------------------------------------------------- */
adb_result_t adb_client_run(void)
{
    s_err = "";

    if (!adb_usb_ready()) { s_err = "usb not ready"; return ADB_RESULT_FAIL; }
    if (!adb_auth_init()) { s_err = "rsa key init"; return ADB_RESULT_FAIL; }
    if (!adb_handshake()) return ADB_RESULT_FAIL;

#if ADB_TCPIP_FIRST
    printf("[adb] 步骤 1/2: adb tcpip %s\n", ADB_CMD_TCPIP_PORT);
    if (do_tcpip() != ADB_RESULT_OK) return ADB_RESULT_FAIL;
    printf("[adb] 步骤 2/2: adb shell\n");
    if (do_shell() != ADB_RESULT_OK) return ADB_RESULT_FAIL;
#else
    printf("[adb] 步骤 1/2: adb shell\n");
    if (do_shell() != ADB_RESULT_OK) return ADB_RESULT_FAIL;
    printf("[adb] 步骤 2/2: adb tcpip %s\n", ADB_CMD_TCPIP_PORT);
    if (do_tcpip() != ADB_RESULT_OK) return ADB_RESULT_FAIL;
#endif

    printf("[adb] DONE — 两步都成功\n");
    return ADB_RESULT_OK;
}
