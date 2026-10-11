/*
 * mbedtls_config.h —— 裁剪后的 mbedtls 用户配置
 *
 * 用法（CMake 里已经帮你加好）：
 *   -DMBEDTLS_USER_CONFIG_FILE="mbedtls_config.h"
 * 它会叠加在 mbedtls 自带的默认配置之后，所以我们只需要"改动量"。
 *
 * 真正把体积压下来的手段是【只编译需要的 .c 文件】（见 CMakeLists.txt 里的
 * MBEDTLS_SRCS 白名单）：
 *      bignum*.c / rsa.c / platform_util.c
 * 哈希、对称加密、X.509、TLS、PEM 一个都不编。
 */
#ifndef ADB_MBEDTLS_USER_CONFIG_H
#define ADB_MBEDTLS_USER_CONFIG_H

/* ============ 保留：ADB AUTH 只用到这三项 ============ */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15

/* AES 不需要（连 AESNI 之类的都别留） */
#undef MBEDTLS_AES_C

/* ============ 关掉：会牵进 PSA / 熵源 / 文件系统的东西 ============ */
#undef MBEDTLS_PSA_CRYPTO_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_BUILTIN_KEYS
#undef MBEDTLS_ENTROPY_C
#undef MBEDTLS_CTR_DRBG_C
#undef MBEDTLS_HMAC_DRBG_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

/* TLS / X.509 / PK 抽象层统统不要 */
#undef MBEDTLS_SSL_TLS_C
#undef MBEDTLS_SSL_CLI_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_PROTO_TLS1_2
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_X509_USE_C
#undef MBEDTLS_X509_CRT_PARSE_C
#undef MBEDTLS_PK_C
#undef MBEDTLS_PK_PARSE_C
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_PEM_PARSE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_ASN1_PARSE_C
#undef MBEDTLS_ASN1_WRITE_C
#undef MBEDTLS_OID_C
#undef MBEDTLS_MD_C

/* 哈希：ADB 的 challenge 是"直接当摘要用"的，我们连哈希实现都不需要 */
#undef MBEDTLS_MD5_C
#undef MBEDTLS_SHA1_C
#undef MBEDTLS_SHA224_C
#undef MBEDTLS_SHA256_C
#undef MBEDTLS_SHA384_C
#undef MBEDTLS_SHA512_C

/* ============ 尺寸调优 ============ */
/* RSA-2048 => 256 字节；把 MPI 上限压到刚好够用 */
#define MBEDTLS_MPI_MAX_SIZE 256

/* 用 CRT 加速私钥运算（默认就是开的；如需再省 flash 可打开下面这行，
 * 代价是签名时间约慢 3~4 倍） */
/* #define MBEDTLS_RSA_NO_CRT */

/* 没有熵源，也不需要随机数（签名是确定性的） */
#define MBEDTLS_NO_PLATFORM_ENTROPY

#endif /* ADB_MBEDTLS_USER_CONFIG_H */
