/*
 * adb_auth.c - 用裁剪版 mbedtls 做 ADB AUTH 的 RSA-2048 签名
 *
 * 算法（与 adbd 的校验严格一致）：
 *
 *   EM = 0x00 || 0x01 || PS(0xFF * 218) || 0x00
 *        || SHA1_DigestInfo(15B) || token(20B)
 *   sig = RSA_private_encrypt(EM)          // 256 字节，大端
 *
 *   其中 SHA1_DigestInfo = 30 21 30 09 06 05 2B 0E 03 02 1A 05 00 04 14
 *   token 就是设备发来的 20 字节 challenge 原文 —— 直接当摘要用，不再做哈希。
 *
 *   （参见 AOSP daemon/auth.cpp: RSA_verify(NID_sha1, token, 20, sig, 256, key)）
 */
#include <string.h>

#include "mbedtls/version.h"
#include "mbedtls/rsa.h"
#include "mbedtls/bignum.h"

#include "adb_auth.h"
#include "adb_key_data.h"

#define RSA_KEY_BYTES 256u
#define ADB_TOKEN_LEN 20u

/* 固定 256 字节 */
uint32_t adb_auth_sig_len(void) { return RSA_KEY_BYTES; }

/* PKCS#1 v1.5 编码时用的 SHA-1 DigestInfo 前缀（ASN.1 DER） */
static const uint8_t SHA1_DIGEST_INFO[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
    0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14,
};

/*
 * 如果哪天你想试 SHA-256 方案（ADB USB AUTH **不会**接受，仅供实验）：
 *   需要把下面这个前缀替换成 SHA256_DIGEST_INFO，并把 token 先用 SHA-256
 *   哈希成 32 字节再放进去，同时 mbedtls 配置里打开 MBEDTLS_SHA256_C。
 */
#if 0
static const uint8_t SHA256_DIGEST_INFO[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};
#endif

static mbedtls_rsa_context s_rsa;
static bool s_ready = false;

/* ------------------------------------------------------------------ */
bool adb_auth_init(void)
{
    if (s_ready) return true;

#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_rsa_init(&s_rsa);
#else
    mbedtls_rsa_init(&s_rsa, MBEDTLS_RSA_PKCS_V15, 0);
#endif

    mbedtls_mpi N, E, D, P, Q;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&D);
    mbedtls_mpi_init(&P);
    mbedtls_mpi_init(&Q);

    int ret = 0;
    if ((ret = mbedtls_mpi_read_binary(&N, ADB_RSA_N, sizeof(ADB_RSA_N))) != 0) goto out;
    if ((ret = mbedtls_mpi_read_binary(&E, ADB_RSA_E, sizeof(ADB_RSA_E))) != 0) goto out;
    if ((ret = mbedtls_mpi_read_binary(&D, ADB_RSA_D, sizeof(ADB_RSA_D))) != 0) goto out;
    if ((ret = mbedtls_mpi_read_binary(&P, ADB_RSA_P, sizeof(ADB_RSA_P))) != 0) goto out;
    if ((ret = mbedtls_mpi_read_binary(&Q, ADB_RSA_Q, sizeof(ADB_RSA_Q))) != 0) goto out;

    /* 导入 N/P/Q/D/E，complete() 会自动推导 DP/DQ/QP，从而走 CRT 加速 */
    if ((ret = mbedtls_rsa_import(&s_rsa, &N, &P, &Q, &D, &E)) != 0) goto out;
    if ((ret = mbedtls_rsa_complete(&s_rsa)) != 0) goto out;

out:
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&D);
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&Q);

    s_ready = (ret == 0);
    return s_ready;
}

/* ------------------------------------------------------------------ */
bool adb_auth_sign(const uint8_t *token, uint32_t token_len, uint8_t *sig_out)
{
    if (!s_ready || token_len != ADB_TOKEN_LEN) return false;

    uint8_t em[RSA_KEY_BYTES];

    const size_t di_len = sizeof(SHA1_DIGEST_INFO) + ADB_TOKEN_LEN; /* 35 */
    const size_t ps_len = RSA_KEY_BYTES - 3 - di_len;               /* 218 */

    memset(em, 0xFF, sizeof(em));
    em[0] = 0x00;
    em[1] = 0x01;
    em[2 + ps_len] = 0x00;
    memcpy(&em[3 + ps_len], SHA1_DIGEST_INFO, sizeof(SHA1_DIGEST_INFO));
    memcpy(&em[3 + ps_len + sizeof(SHA1_DIGEST_INFO)], token, ADB_TOKEN_LEN);

#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_rsa_private(&s_rsa, em, sig_out) == 0;
#else
    /* mbedtls 2.x 需要 f_rng/p_rng；传 NULL 表示不做 blinding */
    return mbedtls_rsa_private(&s_rsa, NULL, NULL, em, sig_out) == 0;
#endif
}

/* ------------------------------------------------------------------ */
const char *adb_auth_public_key_string(void)
{
    return ADB_RSA_PUBKEY_STRING;
}
