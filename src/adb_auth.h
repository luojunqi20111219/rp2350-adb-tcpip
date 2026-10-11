/*
 * adb_auth.h - ADB AUTH 的 RSA-2048 签名
 *
 * 注意（很重要）：
 *   adbd 校验 AUTH SIGNATURE 时执行的是
 *       RSA_verify(NID_sha1, token, 20, sig, 256, pubkey)
 *   也就是说 —— 20 字节的 challenge 被【直接当作 SHA-1 摘要】使用，
 *   外面套 PKCS#1 v1.5 + SHA-1 DigestInfo 前缀，再做 RSA 私钥运算。
 *   全程不出现真正的 SHA-1/SHA-256 哈希计算。
 *
 *   所以本工程用 "RSA PKCS#1 v1.5（SHA-1 DigestInfo）" 方案；
 *   用 SHA-256 方案发给 adbd 一定会被拒绝（那是 adb over TLS 才用的）。
 */
#ifndef ADB_AUTH_H
#define ADB_AUTH_H

#include <stdint.h>
#include <stdbool.h>

/* 初始化：把 Flash 里的私钥导入 mbedtls。只需调用一次。 */
bool adb_auth_init(void);

/*
 * 对 challenge 签名。
 *   token  : 设备发来的 challenge（ADB 固定 20 字节）
 *   sig_out: 输出缓冲区，adb_auth_sig_len() 字节（=256）
 * 返回 true 表示成功。
 */
bool adb_auth_sign(const uint8_t *token, uint32_t token_len, uint8_t *sig_out);

/* 固定 256 字节（RSA-2048） */
uint32_t adb_auth_sig_len(void);

/* ADB AUTH_RSAPUBLICKEY 报文的载荷：base64 公钥 + 注释，末尾带 '\0' */
const char *adb_auth_public_key_string(void);

#endif /* ADB_AUTH_H */
