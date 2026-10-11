/*
 * adb_config.h - 要执行的两条 ADB 命令 / 超时参数
 *
 * 只需要改这个文件就能换命令。
 */
#ifndef ADB_CONFIG_H
#define ADB_CONFIG_H

/* ------------------------------------------------------------------ */
/* ① adb tcpip <port>                                                  */
/*    把 adbd 切到 TCP 模式（方便之后无线连接）                          */
/* ------------------------------------------------------------------ */
#define ADB_CMD_TCPIP_PORT "5555"

/* ------------------------------------------------------------------ */
/* ② adb shell <cmd>                                                   */
/*    用 adb shell 跑的命令，默认是 Shizuku 的启动脚本                   */
/* ------------------------------------------------------------------ */
#define ADB_CMD_SHELL "sh /sdcard/Android/data/moe.shizuku.privileged.api/start.sh"

/* ------------------------------------------------------------------ */
/* 执行顺序                                                            */
/* ------------------------------------------------------------------ */
/*
 * 1 = 先 tcpip 再跑脚本（默认，对应 PC 上 `adb tcpip 5555` → `adb connect`
 *     → `adb shell sh .../start.sh` 的习惯）
 *
 *     注意：tcpip 会让 adbd 重启、USB 断开重新枚举，所以本工程会自动
 *     「等 USB 掉线 → 等它重新枚举 → 重新 CNXN/AUTH 握手」，然后再跑脚本。
 *
 * 0 = 先跑脚本再 tcpip（脚本执行期间 USB 全程不断，最稳；最后 tcpip
 *     断开也就不管了）
 */
#define ADB_TCPIP_FIRST 1

/* ------------------------------------------------------------------ */
/* 超时（毫秒）                                                        */
/* ------------------------------------------------------------------ */
#define ADB_AUTH_MS        3000u  /* 等设备回 AUTH / CNXN */
#define ADB_PUBKEY_MS      60000u /* 发完公钥后，等用户在手机上点“允许” */
#define ADB_SERVICE_MS     5000u  /* 普通服务（tcpip）响应超时 */
#define ADB_SHELL_MS       60000u /* shell 命令最长执行时间 */
#define ADB_UNPLUG_MS      6000u  /* tcpip 之后等 adbd 重启 / USB 掉线 */
#define ADB_REENUM_MS      15000u /* 等 USB 重新枚举完成 */
#define ADB_RETRY_DELAY_MS 300u   /* 重新握手前的小延时 */

/* shell 输出缓存大小（只做日志显示，溢出就截断） */
#define ADB_SHELL_OUT_MAX  768u

#endif /* ADB_CONFIG_H */
