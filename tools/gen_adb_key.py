#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_adb_key.py —— 生成 ADB 用的 RSA-2048 密钥，并产出 C 头文件

产出：
  <out-dir>/adbkey.pem    私钥（PEM）
  <out-dir>/adbkey.pub    公钥（adb .pub 明文格式：base64 + 注释）
  <out-header>            C 头文件：私钥 N/E/D/P/Q 原始大端字节 + 公钥字符串

用法：
  python3 tools/gen_adb_key.py --out-dir tools --out-header src/adb_key_data.h

依赖：优先用 cryptography 库；没有就退回 openssl 命令行（装了 openssl 就行）。
"""

import argparse
import base64
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

PUBKEY_COMMENT = "rp2350-adb@embedded"


# --------------------------------------------------------------------------
# 1. 生成 / 读取密钥
# --------------------------------------------------------------------------
def gen_with_cryptography(bits):
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives import serialization

    key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    nums = key.private_numbers()
    pub = nums.public_numbers
    pem = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.TraditionalOpenSSL,
        encryption_algorithm=serialization.NoEncryption(),
    )
    return {"n": pub.n, "e": pub.e, "d": nums.d, "p": nums.p, "q": nums.q}, pem


def _der_tlv(buf, off):
    tag = buf[off]
    off += 1
    ln = buf[off]
    off += 1
    if ln & 0x80:
        k = ln & 0x7F
        ln = int.from_bytes(buf[off:off + k], "big")
        off += k
    return tag, buf[off:off + ln], off + ln


def der_parse_rsa(der):
    """解析 PKCS#1 RSAPrivateKey 或 PKCS#8 PrivateKeyInfo（DER）"""
    tag, seq, _ = _der_tlv(der, 0)
    if tag != 0x30:
        raise ValueError("不是 SEQUENCE")

    items = []
    off = 0
    while off < len(seq):
        t, v, off = _der_tlv(seq, off)
        items.append((t, v))

    # PKCS#1: version,n,e,d,p,q,dp,dq,qinv  —— 全是 INTEGER
    if len(items) >= 6 and all(t == 0x02 for t, _ in items[:6]):
        ints = [int.from_bytes(v, "big") for _, v in items]
        return {"n": ints[1], "e": ints[2], "d": ints[3], "p": ints[4], "q": ints[5]}

    # PKCS#8: 递归进 OCTET STRING
    for t, v in items:
        if t == 0x04:
            return der_parse_rsa(v)

    raise ValueError("无法识别的私钥格式")


def gen_with_openssl(bits):
    with tempfile.TemporaryDirectory() as td:
        pem_path = os.path.join(td, "k.pem")
        subprocess.run(["openssl", "genrsa", "-out", pem_path, str(bits)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        data = open(pem_path, "rb").read()

        # OpenSSL 3.x 可能输出 PKCS#8，统一转成传统 PKCS#1
        if b"BEGIN RSA PRIVATE KEY" not in data:
            out_path = os.path.join(td, "k2.pem")
            subprocess.run(["openssl", "rsa", "-in", pem_path, "-traditional", "-out", out_path],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            data = open(out_path, "rb").read()

        m = re.search(rb"-----BEGIN [^-]+-----(.+?)-----END", data, re.S)
        if not m:
            raise ValueError("PEM 解析失败")
        der = base64.b64decode(re.sub(rb"\s", b"", m.group(1)))
        return der_parse_rsa(der), data


def get_key(bits):
    try:
        return gen_with_cryptography(bits) + ("cryptography",)
    except ImportError:
        params, pem = gen_with_openssl(bits)
        return params, pem, "openssl"


# --------------------------------------------------------------------------
# 2. 公钥编码
# --------------------------------------------------------------------------
def be(x, size=None):
    if size is None:
        size = max(1, (x.bit_length() + 7) // 8)
    return x.to_bytes(size, "big")


def android_pubkey_blob(n, e):
    """
    AOSP libcrypto_utils/android_pubkey.cpp 的 RSAPublicKey 结构，524 字节：
      uint32 modulus_size_words;                 // 64
      uint32 n0inv;                              // -1/n[0] mod 2^32
      uint8 modulus[256];                        // 小端
      uint8 rr[256];                             // (2^2048)^2 mod n，小端
      uint32 exponent;
    """
    n_le = be(n, 256)[::-1]
    n0 = n & 0xFFFFFFFF
    n0inv = (-pow(n0, -1, 1 << 32)) % (1 << 32)
    rr = pow(1 << 2048, 2, n)
    rr_le = be(rr, 256)[::-1]
    return (struct.pack("<I", 64) + struct.pack("<I", n0inv)
            + n_le + rr_le + struct.pack("<I", e))


def openssh_pubkey_blob(n, e):
    def mpint(x):
        b = be(x)
        if b[0] & 0x80:
            b = b"\x00" + b
        return struct.pack(">I", len(b)) + b

    def s(x):
        return struct.pack(">I", len(x)) + x

    return s(b"ssh-rsa") + mpint(e) + mpint(n)


# --------------------------------------------------------------------------
# 3. 生成 C 头文件
# --------------------------------------------------------------------------
def c_array(name, data, per=12):
    out = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), per):
        out.append("    " + ", ".join("0x%02x" % b for b in data[i:i + per]) + ",")
    out.append("};")
    return "\n".join(out)


def c_string(name, s, per=88):
    parts = [s[i:i + per] for i in range(0, len(s), per)] or [""]
    out = ["static const char %s[] =" % name]
    for i, p in enumerate(parts):
        out.append('    "%s"%s' % (p, ";" if i == len(parts) - 1 else ""))
    return "\n".join(out)


HEADER_TPL = """/*
 * adb_key_data.h —— 由 tools/gen_adb_key.py 自动生成，请勿手改
 *
 * 生成时间：{ts}
 * 密钥来源：{src}
 * 公钥指纹 (MD5 of android_pubkey blob)：{fp}
 *
 * !! 换成你自己的密钥后必须：
 *    1) 重新编译 + 重新烧录固件
 *    2) 手机上"撤销 USB 调试授权"后重新插一次，勾选"始终允许"
 */
#ifndef ADB_KEY_DATA_H
#define ADB_KEY_DATA_H

#include <stdint.h>

/* ---- 私钥参数（大端字节，mbedtls 直接 mpi_read_binary 导入）---- */
{array_n}

{array_e}

{array_d}

{array_p}

{array_q}

/* ---- AUTH_RSAPUBLICKEY 用的公钥字符串（不含结尾 '\\0'）----
 * 默认是 Android 自己的 android_pubkey 格式（524 字节的 base64）；
 * 若定义了 ADB_PUBKEY_OPENSSH 则用标准 OpenSSH "ssh-rsa AAAA..." 格式。
 */
{string_pub}

#endif /* ADB_KEY_DATA_H */
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=None, help="放 adbkey.pem / adbkey.pub 的目录")
    ap.add_argument("--out-header", default=None, help="生成的 C 头文件路径")
    ap.add_argument("--bits", type=int, default=2048)
    ap.add_argument("--comment", default=PUBKEY_COMMENT)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = args.out_dir or here
    out_header = args.out_header or os.path.join(os.path.dirname(here), "src", "adb_key_data.h")

    params, pem_bytes, src = get_key(args.bits)
    n, e, d, p, q = params["n"], params["e"], params["d"], params["p"], params["q"]

    # --- .pem / .pub ---
    os.makedirs(out_dir, exist_ok=True)
    pem_path = os.path.join(out_dir, "adbkey.pem")
    with open(pem_path, "wb") as f:
        f.write(pem_bytes)

    blob = android_pubkey_blob(n, e)
    pub_str = base64.b64encode(blob).decode() + " " + args.comment
    pub_openssh = base64.b64encode(openssh_pubkey_blob(n, e)).decode() + " " + args.comment
    with open(os.path.join(out_dir, "adbkey.pub"), "w") as f:
        f.write(pub_str + "\n")

    fp = ":".join("%02X" % b for b in hashlib.md5(blob).digest())

    # --- C 头 ---
    if not pub_openssh:
        pub_openssh = pub_str

    pub_literal = (
        "#if defined(ADB_PUBKEY_OPENSSH)\n"
        + c_string("ADB_RSA_PUBKEY_STRING", pub_openssh)
        + "\n#else\n"
        + c_string("ADB_RSA_PUBKEY_STRING", pub_str)
        + "\n#endif"
    )

    import datetime
    content = HEADER_TPL.format(
        ts=datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        src=src,
        fp=fp,
        array_n=c_array("ADB_RSA_N", be(n)),
        array_e=c_array("ADB_RSA_E", be(e)),
        array_d=c_array("ADB_RSA_D", be(d)),
        array_p=c_array("ADB_RSA_P", be(p)),
        array_q=c_array("ADB_RSA_Q", be(q)),
        string_pub=pub_literal,
    )

    os.makedirs(os.path.dirname(out_header), exist_ok=True)
    with open(out_header, "w") as f:
        f.write(content)

    print("RSA-%d 密钥已生成 (来源: %s)" % (args.bits, src))
    print("  私钥 : %s" % pem_path)
    print("  公钥 : %s" % os.path.join(out_dir, "adbkey.pub"))
    print("  头文件: %s" % out_header)
    print("  公钥指纹(MD5): %s" % fp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
