/*
 * adb_client.c - 最小 ADB 协议栈
 *
 * 流程（严格对应需求）：
 *   ① CNXN                 -> 发起连接
 *   ② 收到 AUTH(TOKEN)     -> 用内置私钥签名 challenge
 *      发 AUTH(SIGNATURE)
 *   ③ 若签名被拒（设备不认识这把钥匙）-> 发 AUTH(RSAPUBLICKEY)，手机上点“允许”
 *   ④ 收到 CNXN            -> 认证通过
 *   ⑤ OPEN "tcpip:5555"
 *   ⑥ 收到 OKAY            -> 命令执行成功
 *   ⑦ CLSE                 -> 关闭通道
 */
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "adb_proto.h"
#include "adb_client.h"
#include "adb_auth.h"
#include "usbh_adb.h"

#define ADB_LOCAL_ID     1u
#define ADB_READ_MS      3000u
#define ADB_PUBKEY_MS    60000u /* 等用户在手机上点“允许” */

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

    if (!adb_usb_write((const uint8_t *)&s_pkt.msg, (uint16_t)sizeof(adb_msg_t), ADB_READ_MS)) {
        s_err = "tx header";
        return false;
    }
    if (len != 0) {
        if (!adb_usb_write((const uint8_t *)payload, (uint16_t)len, ADB_READ_MS)) {
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
/* 各阶段                                                             */
/* ---------------------------------------------------------------- */
static bool send_cnxn(void)
{
    static const char ID[]     = "host::";
    static const char BANNER[] = "features=shell_v2,cmd,stat_v2,ls_v2";
    char payload[128];
    size_t len = 0;

    memcpy(&payload[len], ID, sizeof(ID));         len += sizeof(ID);     /* "host::\0" */
    memcpy(&payload[len], BANNER, sizeof(BANNER)); len += sizeof(BANNER); /* "features=...\0" */

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
    uint32_t len = (uint32_t)strlen(pk) + 1u; /* 含结尾 '\0' */
    return adb_send(ADB_CMD_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, pk, len);
}

/* ---------------------------------------------------------------- */
adb_result_t adb_client_run(void)
{
    s_err = "";

    if (!adb_usb_ready()) { s_err = "usb not ready"; return ADB_RESULT_FAIL; }
    if (!adb_auth_init()) { s_err = "rsa key init"; return ADB_RESULT_FAIL; }

    printf("[adb] CNXN ...\n");
    if (!send_cnxn()) return ADB_RESULT_FAIL;

    /* ---------- 认证 ---------- */
    bool authenticated = false;
    bool sig_sent      = false;
    bool pubkey_sent   = false;

    for (int guard = 0; guard < 8 && !authenticated; guard++) {
        uint32_t to = pubkey_sent ? ADB_PUBKEY_MS : ADB_READ_MS;
        if (!adb_recv(to)) return ADB_RESULT_FAIL;

        if (s_pkt.msg.command == ADB_CMD_CNXN) {
            printf("[adb] CNXN ok (version=%08x)\n", s_pkt.msg.arg0);
            authenticated = true;
            break;
        }

        if (s_pkt.msg.command == ADB_CMD_AUTH && s_pkt.msg.arg0 == ADB_AUTH_TOKEN) {
            if (!sig_sent) {
                printf("[adb] AUTH token -> sign (RSA-2048/SHA1-DigestInfo)\n");
                if (!send_auth_signature()) return ADB_RESULT_FAIL;
                sig_sent = true;
            } else if (!pubkey_sent) {
                printf("[adb] signature rejected -> send public key, 请手机上勾选“始终允许”\n");
                if (!send_auth_pubkey()) return ADB_RESULT_FAIL;
                pubkey_sent = true;
            } else {
                s_err = "auth rejected";
                return ADB_RESULT_FAIL;
            }
            continue;
        }

        s_err = "unexpected pkt in auth";
        return ADB_RESULT_FAIL;
    }

    if (!authenticated) { s_err = "auth timeout"; return ADB_RESULT_FAIL; }

    /* ---------- OPEN tcpip:5555 ---------- */
    static const char SERVICE[] = "tcpip:5555";
    printf("[adb] OPEN %s\n", SERVICE);
    if (!adb_send(ADB_CMD_OPEN, ADB_LOCAL_ID, 0, SERVICE, (uint32_t)sizeof(SERVICE))) {
        return ADB_RESULT_FAIL;
    }

    bool     ok        = false;
    uint32_t remote_id = 0;

    for (int guard = 0; guard < 16; guard++) {
        if (!adb_recv(ADB_READ_MS)) return ADB_RESULT_FAIL;
        if (s_pkt.msg.arg1 != ADB_LOCAL_ID) continue; /* 不是我们这条流 */

        if (s_pkt.msg.command == ADB_CMD_OKAY) {
            remote_id = s_pkt.msg.arg0;
            ok = true;
            printf("[adb] OKAY (remote=%u) -> tcpip 已生效\n", remote_id);
            break;
        }
        if (s_pkt.msg.command == ADB_CMD_WRTE) {
            remote_id = s_pkt.msg.arg0;
            ok = true;
            printf("[adb] WRTE: %.*s\n", (int)s_pkt.msg.data_length, (const char *)s_pkt.data);
            /* 给设备回个 OKAY 收尾 */
            adb_send(ADB_CMD_OKAY, ADB_LOCAL_ID, remote_id, NULL, 0);
            break;
        }
        if (s_pkt.msg.command == ADB_CMD_CLSE) {
            s_err = "service closed";
            break;
        }
    }

    /* ---------- 关闭通道 ---------- */
    if (remote_id != 0) {
        adb_send(ADB_CMD_CLSE, ADB_LOCAL_ID, remote_id, NULL, 0);
    }

    if (ok) {
        printf("[adb] DONE: 手机已监听 5555\n");
        return ADB_RESULT_OK;
    }
    if (s_err[0] == '\0') s_err = "no OKAY";
    return ADB_RESULT_FAIL;
}
