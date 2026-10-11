# ============================================================================
#  RP2350-USB-CM 脱机 ADB 客户端（adb tcpip 5555）
#  Pico SDK + TinyUSB + Pico-PIO-USB + 裁剪版 mbedtls
# ============================================================================
cmake_minimum_required(VERSION 3.13)

# 默认板卡 = pico2（RP2350）。若你的 SDK 版本不同请改这里或用 -DPICO_BOARD=...
if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico2" CACHE STRING "Pico board name")
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(rp2350_adb_tcpip C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ----------------------------------------------------------------------------
# 可调参数
# ----------------------------------------------------------------------------
set(PIO_USB_DP_PIN "13" CACHE STRING "PIO-USB D+ 引脚（USB-CM=13, USB-C=12）")
option(LED_USE_WS2812 "板载 LED 是 WS2812（关闭则按普通 GPIO LED 处理）" ON)
set(LED_GPIO "16" CACHE STRING "板载 LED 所在 GPIO")
option(ADB_PUBKEY_OPENSSH "AUTH_RSAPUBLICKEY 用 OpenSSH 格式（默认 android_pubkey）" OFF)

set(TARGET rp2350_adb_tcpip)

# ----------------------------------------------------------------------------
# 1) 生成 / 检查 RSA 密钥头文件
#    src/adb_key_data.h 不存在时自动调用 tools/gen_adb_key.py 生成
#    （换密钥后必须重新烧录固件，手机侧也要重新授权）
# ----------------------------------------------------------------------------
set(KEY_HEADER ${CMAKE_CURRENT_LIST_DIR}/src/adb_key_data.h)
if (NOT EXISTS ${KEY_HEADER})
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    message(STATUS "未发现密钥头文件，正在生成 RSA-2048 ADB 密钥 ...")
    execute_process(
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_LIST_DIR}/tools/gen_adb_key.py
                    --out-dir ${CMAKE_CURRENT_LIST_DIR}/tools
                    --out-header ${KEY_HEADER}
            RESULT_VARIABLE gen_rc
            OUTPUT_VARIABLE gen_out
            ERROR_VARIABLE gen_err)
    if (NOT gen_rc EQUAL 0)
        message(FATAL_ERROR "密钥生成失败：\n${gen_out}\n${gen_err}")
    endif ()
    message(STATUS "${gen_out}")
endif ()

# ----------------------------------------------------------------------------
# 2) Pico-PIO-USB
#    优先用 SDK 自带的 tinyusb_pico_pio_usb 目标；
#    否则用 PICO_PIO_USB_PATH 指向的官方源码自己编一个。
# ----------------------------------------------------------------------------
set(PIOUSB_NEEDS_TINYUSB_PORT_SRC OFF)

if (TARGET tinyusb_pico_pio_usb)
    set(PIOUSB_LIB tinyusb_pico_pio_usb)
    message(STATUS "使用 SDK 自带的 tinyusb_pico_pio_usb")
else ()
    if (NOT DEFINED PICO_PIO_USB_PATH AND DEFINED ENV{PICO_PIO_USB_PATH})
        set(PICO_PIO_USB_PATH $ENV{PICO_PIO_USB_PATH})
    endif ()

    if (DEFINED PICO_PIO_USB_PATH AND EXISTS ${PICO_PIO_USB_PATH}/src/pio_usb.c)
        message(STATUS "使用 PICO_PIO_USB_PATH = ${PICO_PIO_USB_PATH}")
        set(PIOUSB_SRC ${PICO_PIO_USB_PATH}/src)

        add_library(pico_pio_usb INTERFACE)
        pico_generate_pio_header(pico_pio_usb ${PIOUSB_SRC}/usb_tx.pio)
        pico_generate_pio_header(pico_pio_usb ${PIOUSB_SRC}/usb_rx.pio)
        target_sources(pico_pio_usb INTERFACE
                ${PIOUSB_SRC}/pio_usb.c
                ${PIOUSB_SRC}/pio_usb_device.c
                ${PIOUSB_SRC}/pio_usb_host.c
                ${PIOUSB_SRC}/usb_crc.c)
        target_link_libraries(pico_pio_usb INTERFACE
                pico_stdlib pico_multicore hardware_pio hardware_dma)
        target_include_directories(pico_pio_usb INTERFACE ${PIOUSB_SRC})

        set(PIOUSB_LIB pico_pio_usb)
        set(PIOUSB_NEEDS_TINYUSB_PORT_SRC ON)
    else ()
        message(FATAL_ERROR
                "找不到 Pico-PIO-USB。请任选其一：\n"
                "  a) 使用带 tinyusb_pico_pio_usb 目标的 pico-sdk（>= 2.0）\n"
                "  b) git clone https://github.com/sekigon-gonnoc/Pico-PIO-USB 后设置\n"
                "     export PICO_PIO_USB_PATH=/path/to/Pico-PIO-USB\n"
                "     或在 cmake 里 -DPICO_PIO_USB_PATH=/path/to/Pico-PIO-USB")
    endif ()
endif ()

# ----------------------------------------------------------------------------
# 3) 裁剪版 mbedtls：只编 RSA 需要的几个 .c
# ----------------------------------------------------------------------------
set(MBEDTLS_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/mbedtls CACHE PATH "mbedtls 源码根目录")
if (NOT EXISTS ${MBEDTLS_DIR}/library/rsa.c)
    message(FATAL_ERROR
            "找不到 mbedtls 源码：${MBEDTLS_DIR}\n"
            "请执行：\n"
            "  git clone --depth 1 -b v3.6.2 https://github.com/Mbed-TLS/mbedtls lib/mbedtls\n"
            "或指定已有路径：-DMBEDTLS_DIR=/path/to/mbedtls")
endif ()

# 白名单：只有这几个文件会被编译 —— 这才是"裁剪"的关键
file(GLOB MBEDTLS_SRCS
        ${MBEDTLS_DIR}/library/bignum.c
        ${MBEDTLS_DIR}/library/bignum_core.c        # 3.6+
        ${MBEDTLS_DIR}/library/bignum_mod.c         # 3.6+
        ${MBEDTLS_DIR}/library/bignum_mod_raw.c     # 3.6+
        ${MBEDTLS_DIR}/library/constant_time.c      # 3.6+
        ${MBEDTLS_DIR}/library/platform_util.c
        ${MBEDTLS_DIR}/library/platform.c
)

add_library(adb_mbedtls STATIC ${MBEDTLS_SRCS})
target_include_directories(adb_mbedtls PUBLIC
        ${MBEDTLS_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/mbedtls)
target_compile_definitions(adb_mbedtls PRIVATE
        MBEDTLS_CONFIG_FILE="adb_mbedtls_config.h")
target_compile_options(adb_mbedtls PRIVATE -Os -w)

# ----------------------------------------------------------------------------
# 4) 主程序
# ----------------------------------------------------------------------------
add_executable(${TARGET}
        ${CMAKE_CURRENT_LIST_DIR}/src/main.c
        ${CMAKE_CURRENT_LIST_DIR}/src/usb_host.c
        ${CMAKE_CURRENT_LIST_DIR}/src/usbh_adb.c
        ${CMAKE_CURRENT_LIST_DIR}/src/adb_client.c
        ${CMAKE_CURRENT_LIST_DIR}/src/adb_auth.c
        ${CMAKE_CURRENT_LIST_DIR}/src/led.c
)

# WS2812 的 PIO 程序 -> 生成 ws2812.pio.h
if (LED_USE_WS2812)
    pico_generate_pio_header(${TARGET} ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)
endif ()

target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_compile_definitions(${TARGET} PRIVATE
        PIO_USB_USE_TINYUSB                                   # 让 PIO-USB 走 TinyUSB 类型
        PIO_USB_DP_PIN_DEFAULT=${PIO_USB_DP_PIN}              # Pico-PIO-USB 的默认引脚宏
        BOARD_PIO_USB_DP_PIN=${PIO_USB_DP_PIN}
        BOARD_LED_GPIO=${LED_GPIO}
        $<$<BOOL:${LED_USE_WS2812}>:LED_USE_WS2812=1>
)

if (ADB_PUBKEY_OPENSSH)
    target_compile_definitions(${TARGET} PRIVATE ADB_PUBKEY_OPENSSH=1)
endif ()

target_link_libraries(${TARGET} PRIVATE
        pico_stdlib
        pico_multicore
        hardware_pio
        hardware_dma
        tinyusb_host
        adb_mbedtls
        ${PIOUSB_LIB}
)

# 如果 PIO-USB 是外部源码引入的，还需要把 TinyUSB 的 pio_usb 端口补上
if (PIOUSB_NEEDS_TINYUSB_PORT_SRC)
    if (NOT DEFINED PICO_TINYUSB_PATH)
        set(PICO_TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)
    endif ()
    target_sources(${TARGET} PRIVATE
            ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c)
endif ()

# 调试串口：UART0 (GP0/GP1)，不开 USB CDC（那块给 PIO-USB 了）
pico_enable_stdio_uart(${TARGET} 1)
pico_enable_stdio_usb(${TARGET} 0)

# 输出 .uf2 / .elf / .hex / .bin
pico_add_extra_outputs(${TARGET})

target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Os)

# 内存占用打印，方便盯着 Flash/SRAM
target_link_options(${TARGET} PRIVATE -Wl,--print-memory-usage)
