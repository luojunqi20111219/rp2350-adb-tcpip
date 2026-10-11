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
 *
 * ---------------------------------------------------------------------------
 * 关于裁剪：这里没有用 mbedtls 的 mbedtls_rsa_* API，而是直接用它的 bignum
 * （MPI）做模幂。原因：
 *
 *   mbedtls 的 rsa.c 会一并拖进 asn1parse/asn1write（DER 导入导出）、
 *   md.c（哈希抽象层）、rsa_alt_helpers.c（参数推导/校验）以及至少一个
 *   哈希实现，合计 20~30 KB Flash —— 而我们真正需要的只是"一次 2048 位
 *   模幂"。这和本工程"控制 Flash/SRAM 占用"的目标直接冲突。
 *
 *   MPI 本来就是 RSA 的运算内核，用它做 PKCS#1 v1.5 签名在数学上完全等价，
 *   而且 mbedtls_mpi_* 的 API 从 2.x 到 3.x 一直没变过，不会有版本适配问题。
 *   签一次约 0.1 s（150 MHz，非 CRT），对一次性认证完全够用。
 * ---------------------------------------------------------------------------
 */
#include <string.h>

#include "mbedtls/bignum.h"

#include "adb_auth.h"
#include "adb_key_data.h"

#define RSA_KEY_BYTES 256u
#define ADB_TOKEN_LEN 20u

/* PKCS#1 v1.5 编码时用的 SHA-1 DigestInfo 前缀（ASN.1 DER） */
static const uint8_t SHA1_DIGEST_INFO[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
    0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14,
};

/*
 * 如果哪天你想试 SHA-256 方案（ADB USB AUTH **不会**接受，仅供实验）：
 *   把上面的前缀换成 SHA256_DIGEST_INFO，并把 token 先 SHA-256 成 32 字节；
 *   同时配置里要打开 MBEDTLS_SHA256_C 并编译 sha256.c。
 *
 *   static const uint8_t SHA256_DIGEST_INFO[19] = {
 *       0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
 *       0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
 *   };
 */

/* 只留签名真正要用的两个大数 */
static mbedtls_mpi s_N;
static mbedtls_mpi s_D;
static bool        s_ready = false;

uint32_t adb_auth_sig_len(void) { return RSA_KEY_BYTES; }

/* ------------------------------------------------------------------ */
bool adb_auth_init(void)
{
    if (s_ready) return true;

    mbedtls_mpi N, E, D, P, Q, T;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&E); mbedtls_mpi_init(&D);
    mbedtls_mpi_init(&P); mbedtls_mpi_init(&Q); mbedtls_mpi_init(&T);

    bool ok = true;

    ok = ok && mbedtls_mpi_read_binary(&N, ADB_RSA_N, sizeof(ADB_RSA_N)) == 0;
    ok = ok && mbedtls_mpi_read_binary(&E, ADB_RSA_E, sizeof(ADB_RSA_E)) == 0;
    ok = ok && mbedtls_mpi_read_binary(&D, ADB_RSA_D, sizeof(ADB_RSA_D)) == 0;
    ok = ok && mbedtls_mpi_read_binary(&P, ADB_RSA_P, sizeof(ADB_RSA_P)) == 0;
    ok = ok && mbedtls_mpi_read_binary(&Q, ADB_RSA_Q, sizeof(ADB_RSA_Q)) == 0;

    if (ok) {
        /* 密钥自检：N 必须是 2048 位，且 P*Q == N（防止密钥生成出错） */
        if (mbedtls_mpi_bitlen(&N) != 2048) ok = false;
        else if (mbedtls_mpi_mul_mpi(&T, &P, &Q) != 0) ok = false;
        else if (mbedtls_mpi_cmp_mpi(&T, &N) != 0) ok = false;
    }

    if (ok) {
        /* 转存成常驻的两个大数（导出成独立 MPI，避免引用临时变量） */
        mbedtls_mpi_init(&s_N);
        mbedtls_mpi_init(&s_D);
        ok = (mbedtls_mpi_copy(&s_N, &N) == 0) &&
             (mbedtls_mpi_copy(&s_D, &D) == 0);
        if (!ok) {
            mbedtls_mpi_free(&s_N);
            mbedtls_mpi_free(&s_D);
        }
    }

    mbedtls_mpi_free(&N); mbedtls_mpi_free(&E); mbedtls_mpi_free(&D);
    mbedtls_mpi_free(&P); mbedtls_mpi_free(&Q); mbedtls_mpi_free(&T);

    s_ready = ok;
    return ok;
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

    mbedtls_mpi M, S;
    mbedtls_mpi_init(&M);
    mbedtls_mpi_init(&S);

    bool ok = false;
    if (mbedtls_mpi_read_binary(&M, em, sizeof(em)) == 0 &&
        /* sig = EM^D mod N —— RSA 私钥运算 */
        mbedtls_mpi_exp_mod(&S, &M, &s_D, &s_N, NULL) == 0 &&
        mbedtls_mpi_write_binary(&S, sig_out, RSA_KEY_BYTES) == 0) {
        ok = true;
    }

    mbedtls_mpi_free(&M);
    mbedtls_mpi_free(&S);
    return ok;
}

/* ------------------------------------------------------------------ */
const char *adb_auth_public_key_string(void)
{
    return ADB_RSA_PUBKEY_STRING;
}
