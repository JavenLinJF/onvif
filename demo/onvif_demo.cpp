/**
 * @file onvif_demo.cpp
 * @brief ONVIF C 风格 API 演示程序（独立测试入口）
 *
 * 依赖：
 *   onvif.h  — ONVIF 库对外 C API 头文件
 *   libonvif.so — ONVIF 库核心实现（编译为动态链接库）
 *
 * 编译：
 *   cd build && cmake .. && make
 *
 * 运行：
 *   ./onvif_demo
 *
 * 部署后使用（仅需三个文件）：
 *   lib/libonvif.so  — 编译好的 .so 库
 *   include/onvif.h  — C API 头文件
 *   demo/onvif_demo  — 可执行演示程序
 */

#include "onvif.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>

// ============================================================================
// Demo 回调函数
// ============================================================================

/** 帧计数器（主码流 / 子码流分开计数） */
static std::atomic<unsigned> g_demo_main_frames{0};
static std::atomic<unsigned> g_demo_sub_frames{0};

/**
 * @brief 演示用码流回调：打印帧信息 + 写入本地文件
 *
 * user_data: 0=主码流，1=子码流
 *
 * 将收到的真实编码帧写入本地文件：
 *   - 主码流：main_00001.264 / .265 / .jpg
 *   - 子码流：sub_00001.264 / .265 / .jpg
 */
static void demo_stream_cb(int index, const uint8_t* data, uint32_t len, onvif_codec_e codec,
                           void* user_data) {
    (void)data;
    int kind = static_cast<int>(reinterpret_cast<intptr_t>(user_data));

    if (kind == 0) {
        unsigned n = ++g_demo_main_frames;
        if (n <= 20) {
            std::cerr << "[onvif] [主码流] 帧 #" << n << " size=" << len
                      << " codec=" << static_cast<int>(codec)
                      << " dev=" << index << '\n';
        }
    } else {
        unsigned n = ++g_demo_sub_frames;
        if (n <= 20) {
            std::cerr << "[onvif] [子码流] 帧 #" << n << " size=" << len
                      << " codec=" << static_cast<int>(codec)
                      << " dev=" << index << '\n';
        }
    }
}

/**
 * @brief 演示用抓拍回调：打印 JPEG 抓拍结果
 */
static void demo_capture_cb(int index, const uint8_t* img, uint32_t img_len, onvif_codec_e codec,
                             void* user_data) {
    (void)img;
    (void)codec;
    int kind = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    const char* tag = (kind == 0) ? "主码流抓拍" : "子码流抓拍";
    std::cerr << "[onvif] [Demo] " << tag << " 成功，JPEG " << img_len << " 字节 (设备 #" << index << ")\n";
}

// ============================================================================
// main
// ============================================================================

/**
 * @brief ONVIF 演示程序入口
 *
 * 完整流程：
 *   1. mg_onvif_client_init        — 初始化 FFmpeg + libcurl
 *   2. mg_onvif_discover_devices   — WS-Discovery 多播发现局域网 ONVIF 设备
 *   3. mg_onvif_start_media_stream — 启动主码流 / 子码流拉流（回调写入 .264 / .265 / .jpg）
 *   4. mg_onvif_capture_image      — 主码流 / 子码流 JPEG 抓拍
 *   5. sleep(5)                    — 等待 5 秒，期间持续输出帧
 *   6. mg_onvif_stop_media_stream — 停止拉流
 *   7. mg_onvif_client_deinit     — 释放资源
 */
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cerr << "[onvif]\n";
    std::cerr << "[onvif] C API 演示程序（独立 demo）：\n";
    std::cerr << "[onvif]   init → discover → 主/子码流拉流 → 主/子抓拍 → 5s 后停止\n";

    // 初始化
    if (mg_onvif_client_init() != ONVIF_OK) {
        std::cerr << "[onvif] [Demo] mg_onvif_client_init 失败\n";
        return 1;
    }

    // 发现设备（最多 5 台，超时 5 秒）
    int count = 5;
    if (mg_onvif_discover_devices(&count, 5000) != ONVIF_OK) {
        std::cerr << "[onvif] [Demo] mg_onvif_discover_devices 失败\n";
        mg_onvif_client_deinit();
        return 1;
    }
    if (count <= 0) {
        std::cerr << "[onvif] [Demo] 未发现设备（检查网络 / ONVIF / WS-Discovery 多播）\n";
        mg_onvif_client_deinit();
        return 1;
    }

    // 启动拉流（user_data: 0=主码流，1=子码流）
    void* tag_main = reinterpret_cast<void*>(static_cast<intptr_t>(0));
    void* tag_sub  = reinterpret_cast<void*>(static_cast<intptr_t>(1));

    onvif_error_e e1 = mg_onvif_start_media_stream(0, ONVIF_STREAM_TYPE_MAIN, demo_stream_cb, tag_main);
    if (e1 != ONVIF_OK)
        std::cerr << "[onvif] [Demo] 主码流未启动: err=" << static_cast<int>(e1) << '\n';

    onvif_error_e e2 = mg_onvif_start_media_stream(0, ONVIF_STREAM_TYPE_SUB, demo_stream_cb, tag_sub);
    if (e2 != ONVIF_OK)
        std::cerr << "[onvif] [Demo] 子码流未启动（可能无第二 Profile）: err=" << static_cast<int>(e2) << '\n';

    // 抓拍
    (void)mg_onvif_capture_image(0, ONVIF_STREAM_TYPE_MAIN, demo_capture_cb, tag_main);
    (void)mg_onvif_capture_image(0, ONVIF_STREAM_TYPE_SUB, demo_capture_cb, tag_sub);

    // 等待 5 秒，期间回调持续输出帧
    std::cerr << "[onvif] [Demo] 拉流进行中，5 秒后停止…\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // 停止并释放
    mg_onvif_stop_media_stream(0);
    mg_onvif_client_deinit();
    std::cerr << "[onvif] [Demo] 结束\n";
    return 0;
}