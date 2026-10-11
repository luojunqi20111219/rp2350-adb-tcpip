/*
 * adb_client.h - 最小 ADB 协议栈（CNXN / AUTH / OPEN / WRTE / CLSE）
 */
#ifndef ADB_CLIENT_H
#define ADB_CLIENT_H

#include <stdbool.h>

typedef enum {
    ADB_RESULT_NONE = 0,
    ADB_RESULT_OK,   /* tcpip 命令执行成功（收到 OKAY） */
    ADB_RESULT_FAIL, /* 认证失败 / 超时 / 被拒 */
} adb_result_t;

/* 跑一遍完整流程：CNXN -> AUTH -> CNXN -> OPEN(tcpip:5555) -> OKAY/CLSE
 * 阻塞执行（内部会 pump USB 栈）。仅应在 adb_usb_ready() 为真时调用一次。 */
adb_result_t adb_client_run(void);

/* 失败原因的简短描述（调试用） */
const char *adb_client_last_error(void);

#endif /* ADB_CLIENT_H */
