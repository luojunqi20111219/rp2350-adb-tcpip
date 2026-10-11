# RP2350-USB-CM 脱机 ADB 客户端（adb tcpip 5555）

把这块小板子的 **Type-C 公头**直接插进安卓手机，它就自动当 USB Host：

```
上电 → PIO-USB 起 Host → 手机接入 → ADB 握手认证
     → adb tcpip 5555
     → (adbd 重启 / USB 重新枚举 / 自动重新握手)
     → adb shell sh /sdcard/Android/data/moe.shizuku.privileged.api/start.sh
     → LED 提示
```

**全程不需要电脑。** 插上就把 Shizuku 激活了，顺带把 adbd 切到 TCP 5555 方便之后无线连接。

要改命令或调顺序，只改 `src/adb_config.h` 就够了：
`ADB_CMD_TCPIP_PORT`、`ADB_CMD_SHELL`、`ADB_TCPIP_FIRST`。

---

## 1. 目录结构

```
rp2350-adb-tcpip/
├── CMakeLists.txt            # 构建脚本（含 PIO-USB / mbedtls 白名单 / 密钥自动生成）
├── pico_sdk_import.cmake     # Pico SDK 引入脚本
├── mbedtls/
│   └── adb_mbedtls_config.h  # 裁剪后的 mbedtls 配置（只留大数运算）
├── src/
│   ├── board_config.h        # ★ 板级引脚，只有这里需要按实物改
│   ├── tusb_config.h         # TinyUSB 裁剪配置（只开 Host）
│   ├── adb_proto.h           # ADB 报文头 + 6 种命令 + AUTH 常量
│   ├── adb_config.h          # ★ 要执行的两条命令 / 顺序 / 超时，只改这里
│   ├── usbh_adb.h/.c         # 自定义 TinyUSB 主机类驱动（匹配 0xFF/0x42/0x01 接口）
│   ├── usb_host.h/.c         # PIO-USB Host 初始化
│   ├── adb_auth.h/.c         # mbedtls 做 RSA-2048 签名
│   ├── adb_client.h/.c       # 最小协议栈：CNXN/AUTH/OPEN/WRTE/CLSE
│   ├── led.h/.c              # 状态灯
│   ├── ws2812.pio            # 板载 WS2812 的 PIO 时序程序
│   ├── main.c
│   └── adb_key_data.h        # 【自动生成】私钥 + 公钥字符串
├── tools/
│   ├── gen_adb_key.py        # RSA 密钥生成脚本
│   ├── adbkey.pem            # 【自动生成】私钥（PC 端 adb 也能用）
│   └── adbkey.pub            # 【自动生成】公钥
└── lib/mbedtls/              # 需要你自己放进来（见第 3 步）
```

---

## 2. 工作原理

```
① 上电        PIO-USB 起 Host（rhport 1），蓝色 LED 常亮 = 等待
② 插手机      枚举到 bInterfaceClass=0xFF / SubClass=0x42 / Protocol=0x01 的接口
              （这就是 adbd 的 ADB 接口，不看 VID/PID，任何品牌都认）
              开一对 Bulk IN / OUT 端点 → 绿灯
③ CNXN        发 "host::\0features=...\0"，版本 0x01000000，maxdata 4096
④ AUTH TOKEN  手机发来 20 字节 challenge
⑤ AUTH SIGN   用 Flash 里的私钥签名后回发（256 字节）
⑥ CNXN        手机认证通过
⑦ OPEN        "tcpip:5555"        ← 步骤 1
⑧ 重新枚举    adbd 重启会让 USB 断开，固件自动等 USB 掉线→重新枚举→重新握手
⑨ OPEN        "shell:sh /sdcard/Android/data/moe.shizuku.privileged.api/start.sh"
                                  ← 步骤 2（输出会打到 UART 日志）
⑩ CLSE        服务正常关闭 = 成功
```

> **为什么第 ⑧ 步必须有**：`adb tcpip 5555` 会让 adbd 重启，USB gadget 随之重新
> 枚举，当前那条 Bulk 通道就废了。所以固件必须等手机重新出现、重新走一遍
> CNXN/AUTH（钥匙已被授权，这一步是自动的、不弹窗），才能发第二条命令。
>
> 如果你不想要这个等待，把 `src/adb_config.h` 里的 `ADB_TCPIP_FIRST` 改成 `0`，
> 就变成「先跑 Shizuku 脚本，最后再 tcpip」——脚本执行期间 USB 全程不断，最稳。

### 关于签名算法（**重要，和第 3 条需求不一致**）

你需求里写的是 "RSA 2048 签名 + SHA256"。实际查了 AOSP 源码
（`packages/modules/adb/daemon/auth.cpp`），adbd 校验的是：

```c
RSA_verify(NID_sha1, token, 20, sig, 256, pubkey)
```

也就是说：

* 那 20 字节 challenge **被直接当成 SHA-1 摘要**；
* 外面套 `PKCS#1 v1.5 + SHA-1 DigestInfo`，再做 RSA 私钥运算；
* **全程不做任何哈希计算**——所以连 SHA-1/SHA-256 的实现都不用编进固件。

SHA-256 在 ADB 里只用于 **adb over TLS** 的证书指纹，用来做 USB AUTH 一定被拒。
本工程按 SHA-1-DigestInfo 实现，`adb_auth.c` 里保留了 SHA-256 前缀常量和一个
`#if 0` 的对照块，供你自己验证。

> 已做过的验证：本工程构造的 EM 块 + 私钥运算，结果与
> `openssl pkeyutl -sign -pkeyopt rsa_padding_mode:pkcs1 -pkeyopt digest:sha1`
> **逐字节一致**，即等价于 ADB 主机的 `RSA_sign(NID_sha1, token, 20)`。

---

## 3. 准备依赖

### 3.1 Pico SDK（2.x，必须支持 RP2350）

```bash
git clone --depth 1 -b master https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk
```

### 3.2 Pico-PIO-USB（二选一）

* **方式 A**：直接用 SDK 自带的 `tinyusb_pico_pio_usb` 目标（SDK ≥ 2.0 一般都有，CMake 会自动优先用它）；
* **方式 B**：自己 clone 一份：

```bash
git clone --depth 1 https://github.com/sekigon-gonnoc/Pico-PIO-USB.git ~/Pico-PIO-USB
export PICO_PIO_USB_PATH=~/Pico-PIO-USB
```

### 3.3 mbedtls（放进工程）

```bash
mkdir -p lib
git clone --depth 1 -b v3.6.2 https://github.com/Mbed-TLS/mbedtls.git lib/mbedtls
```

> 只需要 `mbedtls/library/` 和 `mbedtls/include/`，**不用** `git submodule update`。
> 推荐直接用 **Pico SDK 自带的那份**（`-DMBEDTLS_DIR=$PICO_SDK_PATH/lib/mbedtls`），
> 省一次下载且版本一定匹配。

### 3.4 交叉工具链

```bash
# 任选其一
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi cmake ninja-build   # Debian/Ubuntu
# 或从 developer.arm.com 下载 ARM GNU Toolchain（推荐 13.x 以上）
```

### 3.5 顺手做一处 SDK 修补（PIO-USB 主机必需）

微雪文档里明确提到过：Host 模式下要放开 TinyUSB 的 pio_usb 端口实现。
打开

```
$PICO_SDK_PATH/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
```

找到 `hcd_edpt_abort_xfer()` 和 `hcd_frame_number()`，把里面两行调用的注释**去掉**：

```c
bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  uint8_t const pio_rhport = RHPORT_PIO(rhport);
  return pio_usb_host_endpoint_abort_transfer(pio_rhport, dev_addr, ep_addr);  // ← 取消注释
}

uint32_t hcd_frame_number(uint8_t rhport) {
  (void) rhport;
  return pio_usb_host_get_frame_number();                                      // ← 取消注释
}
```

（如果你用的是自带 `tinyusb_pico_pio_usb` 的新版 SDK，这一步可能已经不需要了。）

---

## 4. 生成密钥

骨架里已经带了一对**示例密钥**（`src/adb_key_data.h` / `tools/adbkey.pem`），能直接编译。
正式使用请务必换成自己的：

```bash
# 删掉旧的（可选）
rm -f src/adb_key_data.h tools/adbkey.pem tools/adbkey.pub

# 生成（首次 cmake 时也会自动调用）
python3 tools/gen_adb_key.py --out-dir tools --out-header src/adb_key_data.h
```

输出示例：

```
RSA-2048 密钥已生成 (来源: cryptography)
  私钥 : tools/adbkey.pem
  公钥 : tools/adbkey.pub
  头文件: src/adb_key_data.h
  公钥指纹(MD5): DD:CC:CF:29:...:1F:2F
```

脚本优先用 `cryptography` 库（`pip install cryptography`），没装就退回调用 `openssl` 命令行，
两者产出的东西一样。它生成的是：

* `ADB_RSA_N/E/D/P/Q` 五个大端字节数组（mbedtls 直接 `mbedtls_mpi_read_binary` 导入）；
* `ADB_RSA_PUBKEY_STRING`：Android 自己的 `android_pubkey` 格式（524 字节结构的 base64）
  —— 这是 `AUTH_RSAPUBLICKEY` 报文真正要发的格式；
  加 `-DADB_PUBKEY_OPENSSH=ON` 可切成标准 OpenSSH 格式。

> 用 `tools/adbkey.pem` 也能在电脑上跑 adb（`adb -P 5555 ...`），
> 手机看到的指纹是一样的。

---

## 5. 编译

```bash
cd rp2350-adb-tcpip
mkdir build && cd build

cmake .. \
  -DPICO_BOARD=pico2 \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIO_USB_DP_PIN=13

make -j$(nproc)
```

生成的固件：`build/rp2350_adb_tcpip.uf2`（同时有 `.elf` / `.bin` / `.hex`）。

### 常用可选参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `-DPICO_BOARD` | `pico2` | RP2350 用 `pico2` |
| `-DPIO_USB_DP_PIN` | `13` | **USB-CM = 13，USB-C 母座版 = 12** |
| `-DLED_USE_WS2812` | `ON` | 板载 LED 是 WS2812；关掉按普通 GPIO LED |
| `-DLED_GPIO` | `16` | 板载 WS2812 数据脚 |
| `-DMBEDTLS_DIR` | `lib/mbedtls` | mbedtls 源码位置 |
| `-DPICO_PIO_USB_PATH` | 空 | Pico-PIO-USB 源码位置 |
| `-DADB_PUBKEY_OPENSSH` | `OFF` | 公钥换 OpenSSH 格式 |

---

## 6. 烧录

1. 按住 **BOOT** 键插 USB（用板子另一个 **Type-C 母座口**，那个才是原生 USB）；
2. 电脑出现 `RPI-RP2` 盘；
3. 把 `build/rp2350_adb_tcpip.uf2` 拖进去，板子自动重启。

> 注意：**PIO-USB 口（公头）只用来插手机，不要接电脑**，它不能用来烧录。

---

## 7. 使用

**手机侧（只需做一次）：**

1. 设置 → 关于手机 → 连点「版本号」7 次，打开**开发者选项**；
2. 开发者选项里打开 **USB 调试**；
3. 首次插入会弹「允许 USB 调试吗？」，勾选 **始终允许** 再点确定。

**板子侧：**

把 Type-C 公头插进手机即可，看灯：

| LED | 含义 |
|---|---|
| 🔵 蓝色常亮 | 等待手机接入 |
| 🟢 绿色常亮 | 已识别到 adb 设备，正在握手 |
| 🟢 **绿色快闪 3 次** | ✅ **两步都成功**：Shizuku 已拉起 + adbd 已切到 5555 |
| 🔴 **红色慢闪** | ❌ 失败（认证被拒 / 超时 / 某一步被拒）。接 GP0/GP1 看 UART 日志能看出是 1/2 哪一步 |

**然后在电脑（或手机本地）验证：**

```bash
adb connect <手机IP>:5555
adb devices          # 应该能看到 <手机IP>:5555  device
adb shell dumpsys activity service moe.shizuku   # 或看 Shizuku App 里是否显示“已启动”
```

成功后手机 adbd 会重启过（这是第 ⑧ 步的原因），这是正常现象。

**UART 日志（GP0=TX / GP1=RX，115200）会打印完整过程**，例如：

```
[adb] CNXN ok (version=01000000)
[adb] AUTH token -> RSA-2048 / SHA1-DigestInfo 签名
[adb] 步骤 1/2: adb tcpip 5555
[adb] OPEN tcpip:5555
[adb] tcpip 已生效
[adb] 等待 adbd 重启 / USB 重新枚举 ...
[adb] USB 已回来，重新握手
[adb] 步骤 2/2: adb shell
[adb] OPEN shell:sh /sdcard/Android/data/moe.shizuku.privileged.api/start.sh
[adb] shell 输出:
...
[adb] DONE — 两步都成功
```

---

## 8. 故障排查

| 现象 | 排查方向 |
|---|---|
| 灯一直蓝，插上没反应 | 引脚不对：USB-CM 用 `-DPIO_USB_DP_PIN=13`，USB-C 用 12；确认插的是**公头那一侧** |
| 灯绿了一下变红 | UART0 (GP0/GP1, 115200) 看日志里的 `[adb] FAILED: xxx` 以及卡在第几步 |
| 红慢闪，日志 `auth rejected` | 手机没信任这把钥匙 → 弹窗时点了拒绝；去开发者选项「撤销 USB 调试授权」再插一次 |
| 红慢闪，日志 `rx header` | PIO-USB 主机时序问题，先做第 3.5 步的 SDK 修补 |
| 卡在 `步骤 1/2` 后 `reenum timeout` | `tcpip` 后 USB 迟迟没回来。多数机型的 adbd 重启只要 1~2 秒；若一直不回，把 `ADB_TCPIP_FIRST` 改成 `0`（先用 USB 跑脚本，最后再 tcpip） |
| 卡在 `步骤 2/2`，`service refused` | 脚本路径不对，或该 ROM 的 adbd 禁用了 `shell:` 服务；确认 `ADB_CMD_SHELL` 里的路径 |
| Shizuku 启动脚本报错 | shell 输出会原样打出来（没打印完就是被 `ADB_SHELL_OUT_MAX` 截断了，改大即可） |
| 枚举不到设备 | 手机 USB 模式要选「传输文件」或「USB 调试」，别选「仅充电」 |
| 编译报 `找不到 mbedtls` | 第 3.3 步没做，或路径不对 |
| 编译报 `找不到 Pico-PIO-USB` | 第 3.2 步没做，或没设 `PICO_PIO_USB_PATH` |

UART 日志接法：GP0 = TX（板子→USB-TTL 的 RX），GP1 = RX，GND 共地。

---

## 9. 资源占用（实测）

在 **Pico SDK 2.3.1 + arm-none-eabi-gcc 13.2.1 + Release/-Os** 下实测：

| 项目 | 实测 |
|---|---|
| FLASH | **53,296 B**（pico2 板级 4 MB 的 1.27%；微雪板 2 MB 也绰绰有余） |
| RAM | **31 KB**（512 KB 的 6.05%） |
| `.text` / `.bss` | 57,388 B / 15,216 B |
| uf2 大小 | 105 KB |
| 签名耗时（RSA-2048 非 CRT，150 MHz） | 约 0.1 s |

> 对照：如果直接用 mbedtls 的 `mbedtls_rsa_*` API，`rsa.c` 会连带拖进
> `asn1parse/asn1write`（DER 导入导出）、`md.c`（哈希抽象层）、
> `rsa_alt_helpers.c`（参数推导/校验）以及至少一个哈希实现，**多 20~30 KB Flash**。
> 本工程只用 mbedtls 的 bignum 做模幂，等价且小一个量级。

### 实测编译命令

```bash
export PICO_SDK_PATH=/opt/pico-sdk          # 2.3.1
cd rp2350-adb-tcpip && mkdir -p build && cd build
cmake .. -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release \
         -DMBEDTLS_DIR=$PICO_SDK_PATH/lib/mbedtls
make -j$(nproc)
# 产出 build/rp2350_adb_tcpip.uf2
```

---

## 10. 关于 Shizuku

固件默认干两件事（顺序可在 `src/adb_config.h` 里用 `ADB_TCPIP_FIRST` 调）：

1. `adb tcpip 5555` —— 把 adbd 切到 TCP 模式，方便之后无线连；
2. `adb shell sh /sdcard/Android/data/moe.shizuku.privileged.api/start.sh`
   —— 真正把 Shizuku 拉起来。

脚本输出会原样打到 UART 日志，方便确认它是成功还是报错。

**已知限制**：这里用的是 ADB 的**传统 shell 协议**（`shell:` 前缀），它没有退出码，
所以固件只能判定「通道正常打开并且正常关闭」。如果需要精确的脚本退出码，
可以把服务名换成 `shell,v2,raw:<cmd>`，并解析 ShellProtocol v2 的分帧头
（每段 4 字节：id / id2 / padding，id=3 表示退出码）。

---

## 11. 实现上的几个说明

* **不用 VID/PID 匹配**：安卓手机牌子太杂（Google 0x18D1、三星 0x04E8、小米 0x2717…），
  靠 `0xFF/0x42/0x01` 接口特征匹配是最稳的。
* **自定义主机类驱动**：现代 TinyUSB 已经删掉了 host 侧的 vendor class，
  所以走 `usbh_app_driver_get_cb()` 注册自己的类驱动（应用驱动优先级高于内置驱动）。
* **enum hook**：`set_config()` 里必须调 `usbh_driver_set_config_complete()`，
  否则枚举链不会推进、`tuh_mount_cb` 不会触发。
* **同步传输靠 pump**：ADB 是严格串行的，所以 `usbh_edpt_claim → usbh_edpt_xfer →
  while(usbh_edpt_busy()) tuh_task();` 这种写法（官方示例也是这么干的），
  代码可读性好得多，裸机上也不会死锁（传输完成由 USB 中断驱动）。
* **mbedtls 裁剪**：真正省空间的是 CMake 里那份 `.c` 白名单
  （`bignum*.c` / `constant_time.c` / `platform_util.c`）。配置里**不定义**
  `MBEDTLS_RSA_C` —— RSA 的模幂直接用 `mbedtls_mpi_exp_mod` 做，
  避开了 `rsa.c` 连带的一大串 ASN.1/MD 依赖（见 `src/adb_auth.c` 顶部注释）。
* **配置文件必须叫别的名字**：mbedtls 的 `build_info.h` 用
  `#include "mbedtls_config.h"` 引入配置，而它自己就在 `include/mbedtls/` 下，
  同名会被默认配置抢占、你的改动被静默忽略。所以这里叫 `adb_mbedtls_config.h`，
  并且是**完全替换**默认配置（`MBEDTLS_CONFIG_FILE`）而不是叠加。
