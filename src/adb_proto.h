/*
 * adb_proto.h - 最小 ADB 协议定义
 *
 * 只保留本工程真正用到的部分：报文头、6 种命令、AUTH 的 3 种 arg0。
 * 参考 AOSP: packages/modules/adb/protocol.txt
 */
#ifndef ADB_PROTO_H
#define ADB_PROTO_H

#include <stdint.h>

/* ---- 命令字（4 字节 ASCII，小端读出）---- */
#define ADB_CMD_CNXN 0x4E584E43u /* "CNXN" */
#define ADB_CMD_AUTH 0x48545541u /* "AUTH" */
#define ADB_CMD_OPEN 0x4E45504Fu /* "OPEN" */
#define ADB_CMD_OKAY 0x59414B4Fu /* "OKAY" */
#define ADB_CMD_CLSE 0x45534C43u /* "CLSE" */
#define ADB_CMD_WRTE 0x45545257u /* "WRTE" */

/* ---- AUTH 的 arg0 取值 ---- */
#define ADB_AUTH_TOKEN       1u  /* 设备 -> 主机：这是 challenge */
#define ADB_AUTH_SIGNATURE   2u  /* 主机 -> 设备：RSA 签名 */
#define ADB_AUTH_RSAPUBLICKEY 3u /* 主机 -> 设备：公钥（触发手机授权弹窗）*/

/* ---- CNXN 版本 / 单包上限 ---- */
#define ADB_VERSION   0x01000000u
#define ADB_MAXDATA   4096u

/* ---- 报文头：24 字节，全部小端 ---- */
typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_crc32;
    uint32_t magic; /* command ^ 0xFFFFFFFF */
} adb_msg_t;

_Static_assert(sizeof(adb_msg_t) == 24, "adb_msg_t must be 24 bytes");

/* 报文体（头部 + 载荷） */
typedef struct {
    adb_msg_t msg;
    uint8_t data[ADB_MAXDATA];
} adb_packet_t;

#define ADB_MAGIC(cmd) ((cmd) ^ 0xFFFFFFFFu)

#endif /* ADB_PROTO_H */
