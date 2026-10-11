/*
 * adb_mbedtls_config.h —— 极致裁剪的 mbedtls 配置（RSA-2048 专用）
 *
 * 用法（CMakeLists.txt 里已加好）：
 *   -DMBEDTLS_CONFIG_FILE="adb_mbedtls_config.h"
 *
 * 注意这里是【完全替换】默认配置，不是叠加，所以不用（也不能用）
 * MBEDTLS_USER_CONFIG_FILE —— 叠加会出现「默认配置开着 AES/PSA/X509，
 * 前置依赖却被抽掉」的情况，check_config.h 会爆一堆 #error。
 *
 * 文件名也别叫 mbedtls_config.h：mbedtls 的 build_info.h 用
 * `#include "mbedtls_config.h"` 引入配置，而它自己就在 include/mbedtls/
 * 目录下，同名会被默认配置抢占、你的改动被静默忽略。
 *
 * 体积真正压下来的手段是【只编需要的 .c】（见 CMakeLists.txt 的 MBEDTLS_SRCS）：
 *     bignum* / rsa.c / oid.c / platform*.c
 * 哈希、对称加密、X.509、TLS、PEM 一个都不编。
 *
 * 为什么连哈希都不需要：ADB 的 20 字节 challenge 是「直接当 SHA-1 摘要用」
 * 的（见 src/adb_auth.c，对应 adbd 的 RSA_verify(NID_sha1, ...)），
 * 我们只用到 SHA-1 的 DigestInfo 常量前缀，不需要哈希实现。
 */
#ifndef ADB_MBEDTLS_CONFIG_H
#define ADB_MBEDTLS_CONFIG_H

/* ---------------- 平台适配 ---------------- */
#define MBEDTLS_PLATFORM_C

/* ---------------- 只保留大数运算 ---------------- */
/* 只用 bignum（MPI）做 RSA-2048 模幂，理由见 src/adb_auth.c 顶部注释。
 * 不启用 MBEDTLS_RSA_C：那会把 rsa.c + asn1parse/asn1write + md.c +
 * 哈希实现整套拖进来，20~30 KB Flash，得不偿失。 */
#define MBEDTLS_BIGNUM_C

/* ---------------- 尺寸调优 ---------------- */
/* RSA-2048 => 256 字节，把 MPI 上限压到刚好够用 */
#define MBEDTLS_MPI_MAX_SIZE 256

/* 模幂窗口：默认 3（表 8 项）。调到 4 快一点，表 16 项 x 256B = 4 KB */
#define MBEDTLS_MPI_WINDOW_SIZE 4

/* 时间/日历：这里刻意不定义 MBEDTLS_HAVE_TIME。
 * 否则 platform_util.c 会去找 mbedtls_ms_time()，交叉编译时会直接 #error。 */

#endif /* ADB_MBEDTLS_CONFIG_H */
