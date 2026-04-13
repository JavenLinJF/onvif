/**
 * @file onvif.cpp
 * @brief 完全自包含的 ONVIF 客户端实现（C 风格 C API）
 *
 * 不依赖任何外部 .h 头文件（标准库和系统头除外）。
 * 所有类型、类、函数均内联在此文件中。
 *
 * ==================================== 文件结构总览 ====================================
 *
 * 头文件引入
 *                 - onvif.h（唯一外部项目头文件，提供 C API 接口）
 *                 - FFmpeg（avformat/avcodec/avutil，extern "C" 包裹）
 *                 - libcurl（HTTP 客户端，抓拍 / SOAP 请求用）
 *                 - 标准库 + 网络套接字（Windows/Linux 兼容）
 *
 * libonvif_client 命名空间 — 内联所有类型和类
 *                 - StreamDataType / OnvifDeviceInfo（内联定义，不依赖外部头文件）
 *                 - tds_* / trt_* 数据结构（Device/Media 服务）
 *                 - IHttpClient / CurlHttpClient（简化版 HTTP 客户端，内联于本文件）
 *
 * 独立工具函数
 *                 - base64_encode（HTTP Basic Auth 编码）
 *                 - build_soap_envelope / build_get_*（SOAP 请求构建）
 *                 - parse_soap_response / parse_get_*（XML 响应解析）
 *
 * OnvifClient / DeviceClient / MediaClient（简化版内联）
 *
 * WS-Discovery 多播设备发现
 *
 * 辅助函数
 *                 - discover_impl（多播发现实现）
 *                 - identify_stream_data_type（FFmpeg codec ID → StreamDataType）
 *                 - fetch_snapshot（HTTP GET 抓图）
 *
 * StreamContext / DeviceContext（拉流上下文）
 *
 * fetch_device_info（完整 ONVIF 认证流程）
 *
 * 对外 C API 实现
 *                 - mg_onvif_client_init / mg_onvif_client_deinit
 *                 - mg_onvif_discover_devices
 *                 - mg_onvif_start_media_stream / mg_onvif_stop_media_stream
 *                 - mg_onvif_capture_image
 *
 * ==================================== 依赖链 ====================================
 *
 *   onvif.h（C API）
 *       ↓
 *   onvif.cpp（完全自包含）
 *       ├── onvif.h（C API 接口声明，唯一外部项目头文件）
 *       ├── FFmpeg（avformat/avcodec/avutil，extern "C" 包裹）
 *       ├── libcurl（HTTP 客户端，抓拍 / SOAP 请求用）
 *       └── WS-Discovery（UDP 多播发现）
 *           └── 所有 ONVIF 类型和类均内联于本文件，不依赖 OnvifStreamPlayer.h
 *
 * ==================================== 编译说明 ====================================
 *
 *   cmake .. -DBUILD_CURL_EXAMPLES=ON
 *   make onvif
 *
 *   依赖：libcurl, libavformat, libavcodec, libavutil, pthread
 *   不依赖：libxml2、OnvifStreamPlayer.h、libonvif_client 库头文件
 */

#include "onvif.h"

extern "C" {
// ============================================================================
// FFmpeg 头文件（均为 C API，需 extern "C" 防止 C++ name mangling）
// ============================================================================

/**
 * libavformat/avformat.h — FFmpeg 封装格式层
 *   负责 RTSP 流打开（avformat_open_input）
 *   流信息探测（avformat_find_stream_info）
 *   读帧（av_read_frame）
 *   关闭流（avformat_close_input）
 */
#include <libavformat/avformat.h>

/**
 * libavcodec/avcodec.h — FFmpeg 编解码层
 *   提供 AVCodecID、AVMediaType 等枚举
 *   av_packet_alloc / av_packet_free（包缓冲管理）
 */
#include <libavcodec/avcodec.h>

/**
 * libavutil/time.h — FFmpeg 工具层
 *   av_usleep（微秒级休眠，比标准 usleep 更精确）
 */
#include <libavutil/time.h>

/**
 * libavutil/dict.h — FFmpeg 字典（AVDictionary）
 *   用于设置 FFmpeg 选项（如 rtsp_transport=tcp）
 */
#include <libavutil/dict.h>
}

// ============================================================================
// libcurl 头文件（纯 C API）
// ============================================================================
//
// curl/curl.h 提供：
//   - curl_global_init / curl_global_cleanup（全局初始化）
//   - curl_easy_init / curl_easy_cleanup（CURL 句柄管理）
//   - curl_easy_setopt（设置选项：URL、回调、超时、认证等）
//   - curl_easy_perform（执行 HTTP 请求）
//   - curl_easy_getinfo（获取响应状态码等）
//   - curl_slist_append / curl_slist_free_all（HTTP 头部管理）
#include <curl/curl.h>

// ============================================================================
// 标准库头文件（C++11 及以上特性）
// ============================================================================
//
// 核心类型：
//   <vector>    — std::vector，存储设备列表、Profile 列表
//   <map>       — std::map，HTTP headers（key-value）、service_endpoints_
//   <set>       — std::set，URL 去重
//   <string>    — std::string，所有字符串处理
//   <sstream>   — std::ostringstream，SOAP XML 字符串拼接
//   <memory>    — std::shared_ptr / std::make_shared，智能指针
//
// 并发与同步：
//   <thread>    — std::thread，后台拉流线程、异步 HTTP 请求线程
//   <mutex>     — std::mutex，保护 g_devices 全局列表
//   <condition_variable> — std::condition_variable，ONVIF 异步回调同步
//   <atomic>    — std::atomic，running / stop_flag 标志位
//
// 函数对象：
//   <functional> — std::function，异步回调类型
//   <chrono>    — std::chrono::seconds，条件变量超时
#include <iostream>
#include <cstring>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <chrono>
#include <cstdint>
#include <cstdlib>

// ============================================================================
// 网络套接字（Windows / Linux 跨平台兼容）
// ============================================================================
//
// WS-Discovery 使用 UDP 多播（239.255.255.250:3702），
// 需要操作系统级别的 socket API，但 Windows 和 Linux 接口不同。
//
// 差异：
//   Windows                         Linux
//   ─────────────────────────────  ─────────────────────────────
//   #include <winsock2.h>          #include <sys/socket.h>
//   #include <ws2tcpip.h>          #include <netinet/in.h>
//   SOCKET（typedef int）           int
//   closesocket()                  close()
//   WSACleanup()                   无
//   WSAStartup()                   无
//   INVALID_SOCKET (-1)            -1
//   SO_RCVTIMEO (setsockopt)      同（结构体 timeval）
//
// 本段代码保证同一份源文件在 Windows / Linux 两个平台均可编译通过。
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    #define CLOSE_SOCKET closesocket   // Windows socket 关闭函数
#else
    #include <sys/socket.h>            // BSD socket API
    #include <netinet/in.h>           // sockaddr_in、htons、inet_addr
    #include <arpa/inet.h>            // inet_ntoa（IP 转字符串）
    #include <unistd.h>               // close()、gethostbyname
    #define CLOSE_SOCKET close        // Linux socket 关闭函数
    #define INVALID_SOCKET -1          // Linux 错误 socket fd
    #define SOCKET_ERROR   -1          // Linux socket 错误码
    typedef int SOCKET;               // Linux 用 int 作为 socket 类型
#endif

// ============================================================================
// libonvif_client 命名空间 — 内联所有需要的类型和类
// ============================================================================
//
// 本命名空间包含了 ONVIF 客户端所需的所有类型和类，全部内联在此文件中。
// 不依赖 OnvifStreamPlayer.h 或 libonvif_client 库中的任何头文件。
//
// 内联内容：
//   1. StreamDataType       — 媒体数据类型（H264/H265/AAC/G711 等）
//   2. OnvifDeviceInfo      — ONVIF 设备完整信息结构体
//   3. tds_*                — Device Service 数据结构（设备信息/能力/主机名）
//   4. trt_*                — Media Service 数据结构（Profile/StreamUri/SnapshotUri）
//   5. tt_VideoEncoding     — 视频编码类型枚举（H264/H265/JPEG）
//   6. tt_StreamType        — 流类型枚举（Unicast/Multicast）
//   7. tt_TransportProtocol — 传输协议枚举（UDP/TCP/RTSP/HTTP）
//   8. ResultWrapper<T>      — ONVIF 异步操作结果封装
//   9. OnvifResult<T>       — shared_ptr<ResultWrapper<T>>，ONVIF 回调类型
//   10. Request / Response  — HTTP 请求/响应数据结构
//   11. Callback            — HTTP 回调函数类型
//   12. IHttpClient         — HTTP 客户端虚接口
//   13. CurlHttpClient      — 基于 libcurl 的 HTTP 客户端实现
//   14. DeviceClient        — ONVIF Device Service 客户端
//   15. MediaClient         — ONVIF Media Service 客户端
//   16. OnvifClient         — ONVIF 主客户端（持有认证信息）
namespace libonvif_client {

// Forward declarations（提前声明，打破 DeviceClient / MediaClient / OnvifClient 之间的循环引用）
//   - DeviceClient 需要知道 OnvifClient 存在（持有 shared_ptr）
//   - MediaClient 需要知道 OnvifClient 存在（持有 shared_ptr）
//   - OnvifClient 需要知道 DeviceClient / MediaClient 存在（创建它们）
class DeviceClient;
class MediaClient;

// ============================================================================
// StreamDataType & OnvifDeviceInfo（内联定义，不依赖 OnvifStreamPlayer.h）
// ============================================================================

/**
 * @brief 媒体数据类型枚举（对应 FFmpeg AVMediaType + AVCodecID）
 *
 * 用法：FFmpeg 拉流后，根据 codec_id 判断数据类型，
 *       再通过 to_onvif_codec() 转换为对外 C API 的 onvif_codec_e。
 *
 * 映射关系：
 *   VIDEO_H264  ← AV_CODEC_ID_H264
 *   VIDEO_H265  ← AV_CODEC_ID_HEVC
 *   VIDEO_JPEG  ← AV_CODEC_ID_MJPEG
 *   AUDIO_AAC   ← AV_CODEC_ID_AAC
 *   AUDIO_G711U ← AV_CODEC_ID_PCM_MULAW（G.711 μ-law）
 *   AUDIO_G711A ← AV_CODEC_ID_PCM_ALAW（G.711 A-law）
 *   AUDIO_PCM   ← AV_CODEC_ID_PCM_S16LE（PCM 原始音频）
 */
enum class StreamDataType : int {
    VIDEO_H264 = 0, VIDEO_H265 = 1, VIDEO_JPEG = 2,   // 视频帧
    AUDIO_AAC = 3, AUDIO_G711U = 4, AUDIO_G711A = 5, // 音频帧
    AUDIO_PCM = 6, UNKNOWN = -1                         // 其他
};

/**
 * @brief ONVIF 设备完整信息结构体
 *
 * 包含从 ONVIF 设备获取的所有元数据：
 *   - 连接信息：device_service_url（设备服务地址）、username / password（认证凭据）
 *   - 码流信息：profile_token、main_stream_uri、sub_stream_uri
 *   - 抓拍信息：main_snapshot_uri、sub_snapshot_uri
 *   - 设备信息：manufacturer、model、firmware_version、serial_number、hardware_id、hostname
 *   - 能力信息：media_xaddr（媒体服务地址）、ptz_xaddr（云台服务地址）
 *   - 编码信息：main_video_codec、sub_video_codec（从 Profile 的 VideoEncoderConfiguration 识别）
 *
 * 生命周期：
 *   发现设备后由 fetch_device_info() 填充 → 存入 DeviceContext → 供拉流和抓拍使用
 *   （全部类型内联于本文件，不依赖 OnvifStreamPlayer.h）
 */
struct OnvifDeviceInfo {
    std::string device_service_url;  // 设备服务 URL（如 http://192.168.1.100:8000/onvif/device_service）
    std::string username = "admin";   // 默认用户名（可被 mg_onvif_set_device_auth 修改）
    std::string password = "admin123";// 默认密码
    std::string profile_token;        // 当前使用的 Profile Token（用于 GetStreamUri）
    std::string main_stream_uri;     // 主码流 RTSP 地址（如 rtsp://192.168.1.100:554/stream1）
    std::string sub_stream_uri;      // 子码流 RTSP 地址（低分辨率，用于预览）
    StreamDataType main_video_codec = StreamDataType::VIDEO_H264;  // 主码流编码类型
    StreamDataType sub_video_codec  = StreamDataType::VIDEO_H264;  // 子码流编码类型
    std::string main_snapshot_uri;  // 主码流 JPEG 快照 URI（用于抓拍）
    std::string sub_snapshot_uri;   // 子码流 JPEG 快照 URI
    std::string manufacturer;        // 设备厂商（如 "HIKVISION"）
    std::string model;              // 设备型号
    std::string firmware_version;    // 固件版本
    std::string serial_number;       // 设备序列号
    std::string hardware_id;        // 硬件 ID
    std::string hostname;           // 设备主机名
    std::string media_xaddr;        // Media 服务端点（GetCapabilities 返回）
    std::string ptz_xaddr;          // PTZ 服务端点（GetCapabilities 返回）
};

// ============================================================================
// 通用结果封装
// ============================================================================

/**
 * @brief ONVIF 异步操作结果封装模板
 *
 * 所有 ONVIF 异步回调都返回 OnvifResult<T>（即 shared_ptr<ResultWrapper<T>>）。
 * ResultWrapper<T> 包含：
 *   - T data         — 成功时的响应数据（如 tds_GetDeviceInformationResponse）
 *   - std::string error_msg — 失败时的错误信息
 *
 * 使用方式：
 *   auto result = make_shared<ResultWrapper<T>>();
 *   if (result->is_error()) { 处理错误 }
 *   else { 使用 result->data }
 */
template<typename T>
struct ResultWrapper {
    T data;                              // 成功时的响应数据
    std::string error_msg;              // 失败时的错误信息（is_error() 检查此字段）
    bool is_error() const { return !error_msg.empty(); }              // 判断是否有错误
    const std::string& get_error_message() const { return error_msg; }  // 获取错误信息
};

/**
 * @brief ONVIF 回调的返回类型（shared_ptr<ResultWrapper<T>>）
 *
 * 使用 shared_ptr 而不是直接返回 ResultWrapper<T> 的原因：
 *   1. 避免拷贝（ResultWrapper 包含 std::vector，拷贝代价高）
 *   2. 可以传递空指针表示"未返回"（区分于"返回但有错误"）
 *   3. 在 lambda 捕获中移动语义更清晰
 */
template<typename T>
using OnvifResult = std::shared_ptr<ResultWrapper<T>>;

// ============================================================================
// tds (Device Service) 数据结构
// ============================================================================
//
// Device Service 是 ONVIF 设备的核心服务，提供设备基本信息查询。
// 每个数据结构对应一个 SOAP 请求和响应。
//
// 调用流程：
//   1. GetServices       → 获取设备支持的所有服务列表
//   2. GetDeviceInformation → 获取厂商/型号/固件/序列号
//   3. GetCapabilities   → 获取各服务的 XAddr 端点
//   4. GetHostname       → 获取设备主机名

/**
 * @brief GetServices 响应 — 设备支持的所有服务地址列表
 *
 * 每个 ONVIF 设备至少支持：
 *   - Device Service（设备本身）
 *   - Media Service（媒体配置）
 *   可选支持：PTZ、Events、Imaging、Analytics 等。
 *
 * 响应中包含每个服务的 namespace URL（如 http://www.onvif.org/ver10/media/wsdl）
 * 和对应的 XAddr（如 http://192.168.1.100/onvif/media_service）。
 */
struct tds_GetServicesResponse {
    std::vector<std::string> Services;  // 各服务的 XAddr 列表
};

/**
 * @brief GetDeviceInformation 响应 — 设备基本信息
 *
 * 通过 tds:GetDeviceInformation SOAP 请求获取，
 * 在 discovery_client 中用于设备识别和展示。
 */
struct tds_GetDeviceInformationResponse {
    std::string Manufacturer;      // 厂商名称
    std::string Model;             // 设备型号
    std::string FirmwareVersion;   // 固件版本
    std::string SerialNumber;      // 设备序列号（唯一标识）
    std::string HardwareId;        // 硬件 ID
};

/**
 * @brief GetCapabilities 响应 — 设备能力集
 *
 * 获取设备支持的功能模块及其服务端点。
 * 响应按功能模块（Category）分类。
 */
struct tds_GetCapabilitiesResponse {
    struct Capabilities {
        struct Media {             // 媒体服务能力
            std::string XAddr;   // Media 服务的 HTTP 端点 URL
        };
        struct PTZ  {             // 云台控制能力
            std::string XAddr;   // PTZ 服务的 HTTP 端点 URL
        };
        Media Media;             // 嵌入而非指针，简化初始化
        PTZ PTZ;
    };
    Capabilities Capabilities;
};

/**
 * @brief GetHostname 响应 — 设备网络主机名
 *
 * 通过 tds:GetHostname 请求获取，
 * Name 字段可能为空（设备未设置主机名）。
 */
struct tds_GetHostnameResponse {
    struct HostnameInformation {
        std::string Name;         // 主机名字符串（如 "IPC"）
        bool FromDHCP = false;    // 是否通过 DHCP 动态获取
    };
    HostnameInformation HostnameInformation;
};

// 类型别名（请求和响应共用同一个结构体，简化 API）
using tds_GetDeviceInformation = tds_GetDeviceInformationResponse;
using tds_GetCapabilities      = tds_GetCapabilitiesResponse;
using tds_GetHostname        = tds_GetHostnameResponse;

// ============================================================================
// trt (Media Service) 数据结构
// ============================================================================
//
// Media Service 负责音视频媒体配置，是拉流和抓拍的核心服务。
//
// 调用流程：
//   1. GetProfiles      → 获取所有 Profile（第 0 个=主码流，第 1 个=子码流）
//   2. GetStreamUri      → 根据 ProfileToken 获取 RTSP 流地址
//   3. GetSnapshotUri     → 获取 JPEG 快照 HTTP 地址

/**
 * @brief 视频编码类型枚举（tt:VideoEncoding）
 *
 * ONVIF 支持的压缩格式：
 *   H264   — 最常用（H.264/AVC）
 *   H265   — 高清低码率（H.265/HEVC）
 *   JPEG   — Motion JPEG（逐帧图片，通常用于子码流或抓拍）
 *   MPEG4  — 老设备可能使用（MPEG-4 Part 2，已淘汰）
 */
enum class tt_VideoEncoding { H264, H265, JPEG, MPEG4 };

/**
 * @brief 流类型枚举（tt:StreamType）
 *
 * RTP_Unicast   — 单播（一对一，IP 摄像机默认）
 * RTP_Multicast — 组播（一对多，需要网络设备支持）
 */
enum class tt_StreamType { RTP_Unicast = 0, RTP_Multicast = 1 };

/**
 * @brief 传输层协议枚举（tt:TransportProtocol）
 *
 * ONVIF 规范中 RTSP 流使用 RTP 封装，传输层协议：
 *   UDP  — 实时性好，但丢包不可恢复（局域网使用）
 *   TCP  — RTP over RTSP（最常用，穿透性好）
 *   RTSP — 即 TCP，等同于 TCP
 *   HTTP — HTTP Tunneling（通过 HTTP 代理，常用于防火墙环境）
 */
enum class tt_TransportProtocol { UDP = 0, TCP = 1, RTSP = 2, HTTP = 3 };

/**
 * @brief 媒体配置文件（tt:Profile）
 *
 * Profile 是 ONVIF 媒体的核心概念，代表一组完整的媒体配置。
 * 通常每个摄像机至少有两个 Profile：
 *   - Profile 0（第 0 个）：主码流（高分辨率、高码率）
 *   - Profile 1（第 1 个）：子码流（低分辨率、低码率）
 *
 * 每个 Profile 包含：
 *   - VideoSourceConfiguration（视频源，如 Camera 1）
 *   - VideoEncoderConfiguration（编码配置，如 H.264 / 1920x1080 / 4096kbps）
 *   - PTZConfiguration（云台控制，可选）
 *   - AudioSourceConfiguration / AudioEncoderConfiguration（音频，可选）
 *   - Metadata（事件元数据，可选）
 *
 * Profile 通过 token（字符串）唯一标识，后续 GetStreamUri 等操作都需要 ProfileToken。
 */
struct tt_Profile {
    std::string Name;           // Profile 名称（人类可读）
    std::string token;          // Profile 唯一标识（用于后续请求）
    struct VideoEncoderConfiguration {
        std::string Name;              // 编码器名称
        std::string token;           // 编码器配置 Token
        tt_VideoEncoding Encoding = tt_VideoEncoding::H264;  // 编码格式
        struct Resolution {            // 分辨率
            int Width = 0;            // 宽度（像素）
            int Height = 0;            // 高度（像素）
        } Resolution;
    };
    VideoEncoderConfiguration VideoEncoderConfiguration;  // 视频编码配置
    struct PTZConfiguration {  // 云台控制配置（可选）
        std::string Name;
        std::string token;
    };
    PTZConfiguration PTZConfiguration;
    struct VideoSourceConfiguration {  // 视频源配置
        std::string token;
    };
    VideoSourceConfiguration VideoSourceConfiguration;
};

struct tt_VideoEncoderConfiguration {
    std::string Name;
    std::string token;
    tt_VideoEncoding Encoding;
    int Width = 0;
    int Height = 0;
};

struct trt_GetProfilesResponse {
    std::vector<tt_Profile> Profiles;  // 设备所有 Profile 列表
};

/**
 * @brief GetStreamUri 响应 — RTSP 流地址
 *
 * MediaUri 包含实时流的完整 RTSP URL。
 * 注意：某些设备的 URI 会在连接后失效（InvalidAfterConnect=true）
 *       或设备重启后失效（InvalidAfterReboot=true），
 *       需要重新调用 GetStreamUri 获取新地址。
 */
struct trt_GetStreamUriResponse {
    struct MediaUri {
        std::string Uri;                    // RTSP 流地址
        bool InvalidAfterConnect = false;  // 连接后会失效
        bool InvalidAfterReboot = false;  // 重启后会失效
        std::string Timeout;              // URI 有效时长（秒）
    };
    MediaUri MediaURI;
};

struct trt_GetSnapshotUriResponse {
    struct MediaUri {
        std::string Uri;  // HTTP GET 快照地址
    };
    MediaUri MediaUri;
};

struct trt_GetStreamUri {
    std::string ProfileToken;        // 指定获取哪个 Profile 的 URI
    struct StreamSetup {
        tt_StreamType Stream = tt_StreamType::RTP_Unicast;  // Unicast / Multicast
        struct Transport {
            tt_TransportProtocol Protocol = tt_TransportProtocol::RTSP;  // 传输协议
        } Transport;
    } StreamSetup;
};

struct trt_GetSnapshotUri {
    std::string ProfileToken;  // 指定获取哪个 Profile 的快照 URI
};

using trt_GetProfiles = trt_GetProfilesResponse;

// ============================================================================
// IHttpClient 接口与数据结构（内联简化版）
// ============================================================================

/**
 * @brief HTTP 请求数据结构
 *
 * 封装一次 HTTP 请求的所有参数。
 */
struct Request {
    std::string url;                          // 请求 URL
    std::string method = "GET";               // HTTP 方法（GET / POST）
    std::string body;                        // 请求体（POST 时使用，SOAP 请求填 XML）
    std::map<std::string, std::string> headers;  // 自定义 HTTP Header
    std::string username;                    // HTTP Basic Auth 用户名
    std::string password;                    // HTTP Basic Auth 密码
};

/**
 * @brief HTTP 响应数据结构
 *
 * 封装一次 HTTP 响应的所有信息。
 */
struct Response {
    int status_code = 0;                      // HTTP 状态码（200=成功，401=未授权，404=未找到）
    std::string body;                       // 响应体（SOAP 响应 XML 或 JPEG 数据）
    std::map<std::string, std::string> headers;  // 响应 Header
    std::string error;                     // 错误信息（CURLcode 错误描述）
};

/**
 * @brief HTTP 回调函数类型
 *
 * CurlHttpClient::request_async 是异步的，HTTP 请求在后台线程执行，
 * 执行完成后通过此回调通知调用者。
 *
 * 使用方式：
 *   client->request_async(req, [&](Response resp) {
 *       if (resp.status_code == 200) { 处理 resp.body }
 *   });
 */
using Callback = std::function<void(Response)>;

/**
 * @brief HTTP 客户端虚接口
 *
 * 定义异步 HTTP 请求的接口，方便后续替换实现（如使用其他 HTTP 库）。
 * 目前只提供了一个实现：CurlHttpClient（基于 libcurl）。
 */
struct IHttpClient {
    virtual ~IHttpClient() = default;
    virtual void request_async(Request request, Callback callback) = 0;  // 异步请求
    virtual void set_ssl_verify(bool) {}       // 是否验证 SSL 证书
    virtual void set_proxy(const std::string&) {}  // 设置代理服务器
    virtual void set_connect_timeout(long) {}  // 设置连接超时
};

// ============================================================================
// CurlHttpClient 实现（内联简化版）
// ============================================================================

/**
 * @brief 基于 libcurl 的简化版异步 HTTP 客户端
 *
 * 与 curl_http_client.h 中的完整版相比，本实现：
 *   ✓ 支持 HTTPS
 *   ✓ 支持 Basic Auth（Authorization: Basic base64(user:pass)）
 *   ✓ 支持自定义 HTTP Header（Content-Type、SOAPAction 等）
 *   ✓ 支持 POST 请求体（SOAP 请求）
 *   ✓ 支持异步回调
 *   ✗ 不使用线程池（每次请求单独 std::thread.detach()）
 *
 * SSL 策略：默认 ssl_verify_=false（ONVIF 设备常使用自签名证书）
 *
 * libcurl 全局状态：
 *   curl_global_init(CURL_GLOBAL_ALL) 在构造函数中执行，static bool 保证只初始化一次。
 *   但由于 curl_global_cleanup() 需要在程序结束时调用，这里没有处理——
 *   简化处理：curl_global_cleanup() 由 mg_onvif_client_deinit() 中的 curl_global_cleanup() 统一处理。
 */
class CurlHttpClient : public IHttpClient {
public:
    /**
     * @brief 构造函数
     *
     * 注意：use_ssl=true 时会设置 SSL_VERIFYPEER / SSL_VERIFYHOST，
     *       但 ssl_verify_ 默认为 false（关闭验证），因为 ONVIF 设备多用自签名证书。
     */
    explicit CurlHttpClient(bool use_ssl = true, size_t /*thread_pool_size*/ = 4)
        : use_ssl_(use_ssl), ssl_verify_(false), connect_timeout_(10) {
        // curl_global_init 必须线程安全调用，static bool 保证全局只初始化一次
        std::lock_guard<std::mutex> lock(curl_mutex_);
        static bool curl_initialized = false;
        if (!curl_initialized) {
            curl_global_init(CURL_GLOBAL_ALL);
            curl_initialized = true;
        }
    }

    ~CurlHttpClient() override = default;

    /**
     * @brief 异步 HTTP 请求（后台线程执行）
     *
     * 使用 std::thread.detach() 启动后台线程，请求完成后通过 callback 回调。
     * detach() 的缺点是没有线程回收，但 ONVIF 请求数量少、生命周期短，可接受。
     */
    void request_async(Request request, Callback callback) override {
        std::thread([this, request = std::move(request), callback = std::move(callback)]() {
            perform_request(request, callback);
        }).detach();
    }

    void set_ssl_verify(bool verify) override { ssl_verify_ = verify; }
    void set_connect_timeout(long timeout_secs) override { connect_timeout_ = timeout_secs; }

private:
    bool use_ssl_;
    bool ssl_verify_;
    size_t thread_pool_size_ = 4;
    long connect_timeout_;
    std::string proxy_;
    std::mutex curl_mutex_;  // 保护 curl_global_init 的 static initialized 标志

    /**
     * @brief 实际执行 HTTP 请求（后台线程中运行）
     *
     * 完整流程：
     * 1. curl_easy_init() 创建 CURL 句柄
     * 2. curl_easy_setopt() 设置所有选项（URL / SSL / 超时 / 认证 / Header）
     * 3. curl_easy_perform() 执行请求
     * 4. curl_easy_getinfo() 获取 HTTP 状态码
     * 5. curl_easy_cleanup() 释放句柄
     * 6. callback(resp) 通知调用者
     */
    void perform_request(const Request& request, Callback callback) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            callback(Response{{}, std::string("Failed to init curl")});  // 构造 status_code=0, body="" 的 Response
            return;
        }

        Response resp;
        std::string response_body;  // 局部 string，callback 时 move 传走

        // 基础选项
        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());

        // SSL 设置（ONVIF 设备常用自签名证书，所以 ssl_verify_=false）
        if (use_ssl_) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, ssl_verify_ ? 1L : 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, ssl_verify_ ? 2L : 0L);
        }

        // 代理设置（目前未使用）
        if (!proxy_.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy_.c_str());
        }

        // 超时设置
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout_);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);       // 禁用 signal（避免线程中被中断）
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // 自动跟随 3xx 重定向
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);      // 最多 5 次重定向

        // HTTP Basic Auth（Authorization: Basic base64(user:pass)）
        if (!request.username.empty()) {
            std::string up = request.username + ":" + request.password;
            curl_easy_setopt(curl, CURLOPT_USERPWD, up.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        }

        // 自定义 HTTP Header（Content-Type、SOAPAction 等）
        struct curl_slist* headers = nullptr;
        for (const auto& h : request.headers) {
            std::string hdr = h.first + ": " + h.second;
            headers = curl_slist_append(headers, hdr.c_str());
        }
        if (headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        // POST 请求体（SOAP 请求用，GET 请求 body 为空）
        if (!request.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        }

        // 写回调：libcurl 接收到数据时追加到 response_body
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

        CURLcode res = curl_easy_perform(curl);  // 阻塞直到请求完成（或超时）

        // 清理 header list（CURL 内部会复制一份）
        if (headers) curl_slist_free_all(headers);

        // 处理结果
        if (res != CURLE_OK) {
            resp.error = curl_easy_strerror(res);  // CURLcode → 错误描述字符串
        } else {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            resp.status_code = static_cast<int>(code);
            resp.body = std::move(response_body);  // move 避免拷贝
        }

        curl_easy_cleanup(curl);
        callback(resp);  // 回调通知请求完成
    }

    /**
     * @brief libcurl 写回调（WriteFunction）
     *
     * curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback)
     * curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response_body)
     *
     * libcurl 在接收到 HTTP 响应数据时，会反复调用此函数。
     * 每次调用传入一个数据块（contents），大小 = size * nmemb。
     * 返回值若不等于 total，则认为写入失败，curl_easy_perform 会返回错误。
     */
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        auto* sb = static_cast<std::string*>(userp);
        sb->append(static_cast<char*>(contents), total);
        return total;
    }
};

}  // namespace libonvif_client

// ============================================================================
// Base64 编码（用于 HTTP Basic Auth）
// ============================================================================

/**
 * @brief 标准 Base64 编码实现
 *
 * HTTP Basic Auth 的 Authorization Header 格式为：
 *   Authorization: Basic base64(username:password)
 * 例如：admin:admin123 → base64 → YWRtaW46YWRtaW4xMjM=
 *
 * Base64 算法：
 *   1. 将输入每 3 字节分成一组（24 bits）
 *   2. 每组拆成 4 个 6-bit 的值（0-63）
 *   3. 每个 6-bit 值查表（A-Z / a-z / 0-9 / + / /）映射为字符
 *   4. 末尾不足 3 字节时，用 '=' 填充到 4 的倍数
 *
 * @param input  原始字符串（如 "admin:admin123"）
 * @return       Base64 编码字符串
 */
static std::string base64_encode(const std::string& input) {
    static const char* codes =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;  // valb：已积累的 bit 数（从右往左计算）
    for (unsigned char c : input) {
        val = (val << 8) + c;   // 将新字节追加到 val（8 bits）
        valb += 8;              // 增加 8 bits
        while (valb >= 0) {    // 每次取 6 bits
            out.push_back(codes[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {           // 处理末尾不足 6 bits 的情况
        out.push_back(codes[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (out.size() % 4) out.push_back('=');  // Base64 要求 4 的倍数
    return out;
}

// ============================================================================
// SOAP 工具函数（全部内联）
// ============================================================================

/**
 * @brief 构建 SOAP 1.2 Envelope（所有 ONVIF SOAP 请求的统一包装）
 *
 * SOAP 1.2 Envelope 结构：
 *   <soap:Envelope>
 *     <soap:Header>
 *       <wsa:Action>...</wsa:Action>        ← SOAPAction HTTP Header 对应
 *       <wsa:MessageID>...</wsa:MessageID>  ← 唯一请求 ID
 *     </soap:Header>
 *     <soap:Body>
 *       <具体请求内容>
 *     </soap:Body>
 *   </soap:Envelope>
 *
 * HTTP 请求格式：
 *   POST /onvif/device_service HTTP/1.1
 *   Host: 192.168.1.100:8000
 *   Content-Type: application/soap+xml; charset=utf-8
 *   SOAPAction: http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation
 *   Authorization: Basic YWRtaW46YWRtaW4xMjM=
 *   [空行]
 *   [SOAP Envelope XML]
 *
 * @param body         SOAP Body 内容（具体请求，如 <tds:GetDeviceInformation/>）
 * @param action       SOAP Action URL（对应 HTTP Header 中的 SOAPAction）
 * @param message_id   唯一消息 ID（如 "uuid:onvif-c-api-xxx"）
 * @return             完整的 SOAP Envelope XML 字符串
 */
static std::string build_soap_envelope(const std::string& body,
                                       const std::string& action,
                                       const std::string& message_id) {
    std::ostringstream oss;
    oss << R"(<?xml version="1.0" encoding="UTF-8"?>)"
        << R"(<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope")"
        << R"( xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing")"
        << R"( xmlns:wsd="http://schemas.xmlsoap.org/ws/2005/04/discovery")"
        << R"( xmlns:tt="http://www.onvif.org/ver10/schema")"
        << R"( xmlns:trt="http://www.onvif.org/ver10/media/wsdl")"
        << R"( xmlns:tds="http://www.onvif.org/ver10/device/wsdl">)"
        << R"(<soap:Header>)"
        << R"(<wsa:Action>)" << action << R"(</wsa:Action>)"
        << R"(<wsa:MessageID>)" << message_id << R"(</wsa:MessageID>)"
        << R"(</soap:Header>)"
        << R"(<soap:Body>)" << body << R"(</soap:Body>)"
        << R"(</soap:Envelope>)";
    return oss.str();
}

/**
 * @brief 构建 GetServices 请求（获取设备所有服务列表）
 *
 * ONVIF 服务发现第一步，调用后获取设备支持的完整服务列表。
 * 简化版实现：不使用 IncludeCapability，减小响应体。
 */
static std::string build_get_services_request() {
    return R"(<tds:GetServices xmlns:tds="http://www.onvif.org/ver10/device/wsdl"><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>)";
}

/**
 * @brief 构建 GetDeviceInformation 请求（获取设备基本信息）
 *
 * 返回厂商/型号/固件/序列号，响应示例：
 *   <tds:GetDeviceInformationResponse>
 *     <tds:Manufacturer>HIKVISION</tds:Manufacturer>
 *     <tds:Model>DS-2CD3...</tds:Model>
 *     ...
 *   </tds:GetDeviceInformationResponse>
 */
static std::string build_get_device_info_request() {
    return R"(<tds:GetDeviceInformation xmlns:tds="http://www.onvif.org/ver10/device/wsdl"/>)";
}

/**
 * @brief 构建 GetCapabilities 请求（获取设备能力）
 *
 * Category=All 返回所有能力模块的 XAddr：
 *   - Device / Media / PTZ / Events / Analytics
 *
 * 响应示例：
 *   <tds:GetCapabilitiesResponse>
 *     <tds:Capabilities>
 *       <tt:Media>
 *         <tt:XAddr>http://192.168.1.100/onvif/media_service</tt:XAddr>
 *       </tt:Media>
 *     </tds:Capabilities>
 *   </tds:GetCapabilitiesResponse>
 */
static std::string build_get_capabilities_request() {
    return R"(<tds:GetCapabilities xmlns:tds="http://www.onvif.org/ver10/device/wsdl"><tds:Category>All</tds:Category></tds:GetCapabilities>)";
}

/** @brief 构建 GetHostname 请求 */
static std::string build_get_hostname_request() {
    return R"(<tds:GetHostname xmlns:tds="http://www.onvif.org/ver10/device/wsdl"/>)";
}

/**
 * @brief 构建 GetProfiles 请求（获取所有媒体配置）
 *
 * GetProfiles 是 ONVIF 媒体中最核心的接口，返回设备所有 Profile。
 * ONVIF 规范要求所有设备至少支持一个 Profile。
 *
 * 响应包含每个 Profile 的完整配置（编码类型、分辨率、码率、帧率等）。
 */
static std::string build_get_profiles_request(const std::string& /*media_xaddr*/) {
    return R"(<trt:GetProfiles xmlns:trt="http://www.onvif.org/ver10/media/wsdl"/>)";
}

/**
 * @brief 构建 GetStreamUri 请求（获取 RTSP 流地址）
 *
 * 传入 ProfileToken 指定 Profile，设置 Stream=RTP-Unicast，Protocol=RTSP。
 *
 * 响应示例：
 *   <trt:GetStreamUriResponse>
 *     <trt:MediaUri>
 *       <tt:Uri>rtsp://192.168.1.100:554/Streaming/Channels/101?transportmode=unicast</tt:Uri>
 *       <tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>
 *       <tt:Timeout>PT60S</tt:Timeout>
 *     </trt:MediaUri>
 *   </trt:GetStreamUriResponse>
 *
 * @param profile_token  指定 Profile 的 Token
 * @param stream_type    "RTP-Unicast" 或 "RTP-Multicast"
 */
static std::string build_get_stream_uri_request(const std::string& /*media_xaddr*/,
                                                  const std::string& profile_token,
                                                  const std::string& stream_type) {
    std::ostringstream oss;
    oss << R"(<trt:GetStreamUri xmlns:trt="http://www.onvif.org/ver10/media/wsdl">)"
        << R"(<trt:ProfileToken>)" << profile_token << R"(</trt:ProfileToken>)"
        << R"(<trt:StreamSetup>)"
        << R"(<tt:Stream>)" << stream_type << R"(</tt:Stream>)"
        << R"(<tt:Transport>)"
        << R"(<tt:Protocol>RTSP</tt:Protocol>)"
        << R"(</tt:Transport>)"
        << R"(</trt:StreamSetup>)"
        << R"(</trt:GetStreamUri>)";
    return oss.str();
}

/**
 * @brief 构建 GetSnapshotUri 请求（获取 JPEG 快照 URI）
 *
 * 部分设备只支持主码流抓拍，子码流可能不支持。
 * GetSnapshotUri 返回的 URL 通常是 HTTP GET，下载即为 JPEG 数据。
 */
static std::string build_get_snapshot_uri_request(const std::string& /*media_xaddr*/,
                                                    const std::string& profile_token) {
    std::ostringstream oss;
    oss << R"(<trt:GetSnapshotUri xmlns:trt="http://www.onvif.org/ver10/media/wsdl">)"
        << R"(<trt:ProfileToken>)" << profile_token << R"(</trt:ProfileToken>)"
        << R"(</trt:GetSnapshotUri>)";
    return oss.str();
}

// ============================================================================
// XML 解析工具（简化的字符串提取）
// ============================================================================
//
// 设计决策：不使用 libxml2，直接用 std::string::find 进行简单的标签提取。
// 优点：无需依赖 libxml2，编译更简单。
// 缺点：无法处理命名空间混用、属性顺序变化、CDATA 等复杂 XML 场景。
// 实际 ONVIF 设备通常返回格式规范的 XML，本简化实现足够覆盖常见设备。
//
// 命名空间兼容性处理：
//   不同厂商返回的 XML 命名空间前缀可能不同：
//     <wsdd:XAddrs>          （标准 WS-Discovery）
//     <XAddrs>               （简化格式）
//     <ns2:XAddr>            （某些 Axis/Hikvision 设备）
//   本实现用 find 顺序尝试多种格式，保证兼容性。

/**
 * @brief 从 XML 中提取指定标签的文本内容
 *
 * 核心逻辑：找到 <tag>...</tag> 之间的内容。
 *
 * 命名空间兼容：先尝试精确匹配 <tag>，找不到则去掉前缀匹配 <LocalName>。
 *   例如：<wsdd:XAddrs>http://...</wsdd:XAddrs>
 *         → 先找 <wsdd:XAddrs> → 找不到则找 <XAddrs>
 *
 * @param xml  原始 XML 字符串
 * @param tag  标签名（可含命名空间前缀，如 "wsdd:XAddrs"）
 * @return     标签内的文本内容，查找失败返回空字符串
 */
/**
 * 设备常返回带命名空间的元素（如 tds:Manufacturer、tt:Uri），仅匹配无前缀标签会失败。
 */
static std::string extract_xml_tag(const std::string& xml, const std::string& tag) {
    auto extract_one = [](const std::string& x, const std::string& t) -> std::string {
        std::string open = "<" + t;
        std::string close = "</" + t + ">";
        size_t a = x.find(open);
        if (a == std::string::npos) return "";
        a = x.find(">", a);
        if (a == std::string::npos) return "";
        a++;
        size_t b = x.find(close, a);
        if (b == std::string::npos) return "";
        return x.substr(a, b - a);
    };

    if (tag.find(':') != std::string::npos) {
        std::string r = extract_one(xml, tag);
        if (!r.empty()) return r;
        return extract_one(xml, tag.substr(tag.find(':') + 1));
    }

    static const char* k_ns[] = {"tds:", "tt:", "trt:", "s:", "wsnt:", "timg:", "tev:", ""};
    for (const char* p : k_ns) {
        std::string full = (p && p[0]) ? std::string(p) + tag : tag;
        std::string r = extract_one(xml, full);
        if (!r.empty()) return r;
    }
    return "";
}

static std::string extract_xml_attr(const std::string& xml, const std::string& open_tag,
                                     const std::string& attr) {
    size_t a = xml.find(open_tag);
    if (a == std::string::npos) return "";
    size_t end = xml.find(">", a);
    if (end == std::string::npos) return "";
    std::string header = xml.substr(a, end - a);
    std::string search = attr + "=\"";
    size_t p = header.find(search);
    if (p == std::string::npos) return "";
    p += search.size();
    size_t q = header.find("\"", p);
    if (q == std::string::npos) return "";
    return header.substr(p, q - p);
}

static std::string parse_soap_response(const std::string& xml) {
    size_t a = xml.find("<soap:Body>");
    if (a == std::string::npos) {
        a = xml.find("<s:Body>");
        if (a == std::string::npos) {
            a = xml.find("<SOAP-ENV:Body>");
            if (a == std::string::npos) return "";
            a += 14;
        } else {
            a += 7;
        }
    } else {
        a += 12;
    }
    size_t b = xml.find("</soap:Body>", a);
    if (b == std::string::npos) {
        b = xml.find("</s:Body>", a);
        if (b == std::string::npos)
            b = xml.find("</SOAP-ENV:Body>", a);
    }
    if (b == std::string::npos) return "";
    return xml.substr(a, b - a);
}

static bool parse_get_services_response(const std::string& xml,
                                         std::vector<std::string>& service_xaddrs) {
    std::string body = parse_soap_response(xml);
    if (body.empty()) return false;

    size_t pos = 0;
    while (true) {
        size_t a = body.find("<ns2:XAddr>", pos);
        if (a == std::string::npos) {
            a = body.find("<tds:XAddr>", pos);
        }
        if (a == std::string::npos) {
            a = body.find("<tt:XAddr>", pos);
        }
        if (a == std::string::npos) {
            a = body.find("<XAddr>", pos);
        }
        if (a == std::string::npos) break;
        a = body.find(">", a);
        if (a == std::string::npos) break;
        a++;
        size_t b = body.find("</", a);
        if (b == std::string::npos) break;
        service_xaddrs.push_back(body.substr(a, b - a));
        pos = b;
    }
    return !service_xaddrs.empty();
}

static bool parse_get_device_info_response(const std::string& xml,
                                            std::string& manufacturer,
                                            std::string& model,
                                            std::string& firmware,
                                            std::string& serial,
                                            std::string& hardware) {
    std::string body = parse_soap_response(xml);
    if (body.empty()) return false;
    manufacturer = extract_xml_tag(body, "Manufacturer");
    model        = extract_xml_tag(body, "Model");
    firmware     = extract_xml_tag(body, "FirmwareVersion");
    serial       = extract_xml_tag(body, "SerialNumber");
    hardware     = extract_xml_tag(body, "HardwareId");
    return !manufacturer.empty();
}

static bool parse_get_capabilities_response(const std::string& xml,
                                             std::string& media_xaddr,
                                             std::string& ptz_xaddr) {
    std::string body = parse_soap_response(xml);
    if (body.empty()) return false;

    auto xaddr_in_block = [&body](size_t block_start, const char* close_tag) -> std::string {
        size_t end = body.find(close_tag, block_start);
        if (end == std::string::npos) return "";
        std::string chunk = body.substr(block_start, end - block_start);
        return extract_xml_tag(chunk, "XAddr");
    };

    size_t ma = body.find("<tt:Media");
    if (ma == std::string::npos) ma = body.find("<Media>");
    if (ma != std::string::npos) {
        media_xaddr = xaddr_in_block(ma, "</tt:Media>");
        if (media_xaddr.empty()) media_xaddr = xaddr_in_block(ma, "</Media>");
    }

    size_t pa = body.find("<tt:PTZ");
    if (pa == std::string::npos) pa = body.find("<PTZ>");
    if (pa != std::string::npos) {
        ptz_xaddr = xaddr_in_block(pa, "</tt:PTZ>");
        if (ptz_xaddr.empty()) ptz_xaddr = xaddr_in_block(pa, "</PTZ>");
    }
    return !media_xaddr.empty();
}

static bool parse_get_hostname_response(const std::string& xml,
                                          std::string& hostname) {
    std::string body = parse_soap_response(xml);
    if (body.empty()) return false;
    hostname = extract_xml_tag(body, "Name");
    return !hostname.empty();
}

static bool parse_get_profiles_response(const std::string& xml,
                                          std::vector<libonvif_client::tt_Profile>& profiles) {
    std::string body = parse_soap_response(xml);
    if (body.empty()) return false;

    size_t pos = 0;
    while (true) {
        size_t a = body.find("<trt:Profiles", pos);
        if (a == std::string::npos) {
            a = body.find("<Profiles", pos);
            if (a == std::string::npos) break;
        }
        std::string token = extract_xml_attr(body.substr(a), "<trt:Profiles", "token");
        if (token.empty()) token = extract_xml_attr(body.substr(a), "<Profiles", "token");

        size_t next_a = body.find("<trt:Profiles", a + 1);
        if (next_a == std::string::npos) next_a = body.find("<Profiles", a + 1);
        std::string prof_xml =
            (next_a != std::string::npos) ? body.substr(a, next_a - a) : body.substr(a);

        libonvif_client::tt_Profile prof;
        prof.token = token;
        prof.Name  = extract_xml_tag(prof_xml, "Name");

        size_t vc_a = prof_xml.find("<tt:VideoEncoderConfiguration");
        if (vc_a == std::string::npos) vc_a = prof_xml.find("<VideoEncoderConfiguration");
        if (vc_a == std::string::npos) vc_a = prof_xml.find("<VideoEncoderConfiguration>", 0);
        if (vc_a != std::string::npos) {
            std::string enc_str = extract_xml_tag(prof_xml.substr(vc_a), "Encoding");
            if (enc_str == "H264") prof.VideoEncoderConfiguration.Encoding = libonvif_client::tt_VideoEncoding::H264;
            else if (enc_str == "H265") prof.VideoEncoderConfiguration.Encoding = libonvif_client::tt_VideoEncoding::H265;
            else if (enc_str == "JPEG") prof.VideoEncoderConfiguration.Encoding = libonvif_client::tt_VideoEncoding::JPEG;
        }

        profiles.push_back(prof);
        pos = a + 1;
        if (profiles.size() > 20) break;
    }
    return !profiles.empty();
}

static bool parse_get_stream_uri_response(const std::string& xml,
                                            std::string& uri) {
    std::string body = parse_soap_response(xml);
    if (body.empty()) return false;
    uri = extract_xml_tag(body, "Uri");
    return !uri.empty();
}

static bool parse_get_snapshot_uri_response(const std::string& xml,
                                              std::string& uri) {
    std::string body = parse_soap_response(xml);
    if (body.empty()) return false;
    uri = extract_xml_tag(body, "Uri");
    return !uri.empty();
}

// ============================================================================
// OnvifClient / DeviceClient / MediaClient（简化版内联实现）
// ============================================================================
//
// 这三个类构成了简化版的 ONVIF 客户端 SDK，
// 完全内联，不依赖 libonvif_client 外部库。
//
// 类之间的关系：
//   OnvifClient（持有）
//     ├── 持有 DeviceClient ← create_device_client()
//     ├── 持有 MediaClient ← create_service<MediaClient>()
//     └── 持有 IHttpClient（CurlHttpClient）
//
//   DeviceClient（弱持有）
//     └── weak_ptr<OnvifClient> ← 需要 OnvifClient 发送请求
//
//   MediaClient（弱持有）
//     └── weak_ptr<OnvifClient> ← 需要 OnvifClient 发送请求
//
// 为什么用 weak_ptr：
//   DeviceClient 和 MediaClient 被 OnvifClient 的 shared_ptr 持有，
//   若再让它们持有 shared_ptr<OnvifClient>，会形成循环引用（shared_ptr cycle），
//   导致引用计数永远不为 0，资源无法释放。
//   使用 weak_ptr 打破循环：子对象不增加父对象的引用计数。

namespace libonvif_client {

/**
 * @brief 生成唯一的 SOAP MessageID
 *
 * ONVIF SOAP 规范要求每个请求有唯一的 MessageID，
 * 格式为 "uuid:xxx"，实际只需保证每次请求不同。
 *
 * 实现：使用时间戳（十六进制）+ 递增计数器。
 */
static std::string make_message_id() {
    static int counter = 0;
    std::ostringstream oss;
    oss << "uuid:onvif-c-" << std::hex << std::time(nullptr) << "-" << std::hex << (++counter);
    return oss.str();
}

/**
 * @brief ONVIF 主客户端
 *
 * 核心职责：
 *   1. 持有设备连接信息（URL / 认证凭据）
 *   2. 持有 HTTP 客户端（IHttpClient）
 *   3. 构建并发送所有 SOAP 请求
 *   4. 维护 service_endpoints_（从 GetServices 填充，Media/PTZ 等服务的 XAddr）
 *
 * 继承 std::enable_shared_from_this：
 *   使得 OnvifClient 可以将自己（shared_ptr）传递给 DeviceClient / MediaClient，
 *   DeviceClient / MediaClient 持有 weak_ptr<OnvifClient>。
 *
 * 使用方式：
 *   auto client = make_shared<OnvifClient>(url, user, pwd, http_client);
 *   client->initialize(cb);              // 初始化（GetServices）
 *   auto dev = client->create_device_client();  // 获取设备客户端
 *   auto media = client->create_service<MediaClient>();  // 获取媒体客户端
 */
class OnvifClient : public std::enable_shared_from_this<OnvifClient> {
public:
    OnvifClient(const std::string& device_url,
                const std::string& username,
                const std::string& password,
                std::shared_ptr<IHttpClient> http_client)
        : device_url_(device_url), username_(username),
          password_(password), http_client_(http_client) {}

    using ServicesCallback = std::function<void(OnvifResult<tds_GetServicesResponse>)>;
    using Callback = std::function<void(const std::string& xml)>;

    /**
     * @brief 初始化 ONVIF 会话
     *
     * 调用 GetServices，填充 service_endpoints_（Media / PTZ 等服务的 XAddr）。
     * 这是所有 ONVIF 操作的必要前置步骤。
     *
     * @param cb  异步回调，返回服务列表
     */
    void initialize(ServicesCallback cb) {
        std::string body = build_get_services_request();
        std::string action = "http://www.onvif.org/ver10/device/wsdl/GetServices";
        send_request(device_url_, action, body,
            [this, cb](const std::string& xml) {
                auto result = std::make_shared<ResultWrapper<tds_GetServicesResponse>>();
                if (!parse_soap_response(xml).empty()) {
                    parse_get_services_response(xml, result->data.Services);
                    // 从 GetServices 响应中提取各服务的 XAddr
                    for (const auto& addr : result->data.Services) {
                        size_t ns_pos = addr.find("http://");
                        if (ns_pos != std::string::npos) {
                            size_t path_pos = addr.find("/");
                            if (path_pos != std::string::npos) {
                                std::string ns = addr.substr(ns_pos);
                                if (ns.find("/media") != std::string::npos)
                                    service_endpoints_["http://www.onvif.org/ver10/media/wsdl"] = ns;
                                else if (ns.find("/ptz") != std::string::npos)
                                    service_endpoints_["http://www.onvif.org/ver10/ptz/wsdl"] = ns;
                            }
                        }
                    }
                }
                cb(result);
            });
    }

    /** @brief 创建 DeviceClient */
    std::shared_ptr<DeviceClient> create_device_client() {
        return std::make_shared<DeviceClient>(shared_from_this());
    }

    /**
     * @brief 创建指定服务的客户端
     *
     * 目前仅支持 MediaClient。
     * 若 GetServices 返回了 Media 服务 XAddr，使用它；
     * 否则 fallback 到 device_url 路径替换为 /onvif/media_service。
     */
    template<typename T>
    std::shared_ptr<T> create_service() {
        static_assert(std::is_same<T, MediaClient>::value, "only MediaClient supported");
        std::string media_ns = "http://www.onvif.org/ver10/media/wsdl";
        auto it = service_endpoints_.find(media_ns);
        if (it == service_endpoints_.end()) {
            // Fallback：从 device_url 推断 media_service 地址
            std::string fallback = device_url_;
            size_t pos = fallback.find("/onvif");
            if (pos != std::string::npos)
                fallback = fallback.substr(0, pos) + "/onvif/media_service";
            return std::make_shared<MediaClient>(shared_from_this(), fallback);
        }
        return std::make_shared<MediaClient>(shared_from_this(), it->second);
    }

    /**
     * @brief 发送 SOAP 请求（核心方法）
     *
     * 完整流程：
     * 1. build_soap_envelope() → 包装 XML
     * 2. base64_encode(username:password) → HTTP Basic Auth
     * 3. 构造 Request（URL + body + headers + auth）
     * 4. http_client_->request_async() → 异步发送
     * 5. HTTP 响应到达后，提取 body，调用回调
     *
     * @param url         目标服务 URL（device_url 或 media_xaddr）
     * @param soap_action SOAPAction HTTP Header
     * @param body        SOAP Envelope XML
     * @param cb          HTTP 响应回调（传入 XML 字符串）
     */
    void send_request(const std::string& url,
                      const std::string& soap_action,
                      const std::string& body,
                      Callback cb) {
        std::string msg_id = make_message_id();
        std::string envelope = build_soap_envelope(body, soap_action, msg_id);
        std::string auth = base64_encode(username_ + ":" + password_);

        Request req;
        req.url = url;
        req.body = envelope;
        req.headers["Content-Type"] = "application/soap+xml; charset=utf-8";
        req.headers["SOAPAction"] = soap_action;
        req.headers["Authorization"] = "Basic " + auth;
        req.username = username_;  // 也通过 CURLOPT_USERPWD 设置
        req.password = password_;

        http_client_->request_async(std::move(req),
            [cb](Response resp) { cb(resp.body); });
    }

    std::string device_url_;
    std::string username_;
    std::string password_;
    std::shared_ptr<IHttpClient> http_client_;
    std::map<std::string, std::string> service_endpoints_;  // namespace → XAddr
    std::mutex mutex_;
};

// ============================================================================
// DeviceClient
// ============================================================================

/**
 * @brief ONVIF Device Service 客户端
 *
 * 提供设备基本信息查询接口：
 *   GetDeviceInformation — 厂商/型号/固件/序列号
 *   GetCapabilities     — Media / PTZ / Events 等服务地址
 *   GetHostname        — 设备主机名
 *
 * DeviceClient 持有 weak_ptr<OnvifClient>，通过 OnvifClient::send_request 发送请求。
 * weak_ptr 打破循环引用，避免内存泄漏。
 */
class DeviceClient {
public:
    explicit DeviceClient(std::shared_ptr<OnvifClient> client) : onvif_client_(client) {}

    using GetDeviceInfoCb  = std::function<void(OnvifResult<tds_GetDeviceInformationResponse>)>;
    using GetCapabilitiesCb = std::function<void(OnvifResult<tds_GetCapabilitiesResponse>)>;
    using GetHostnameCb    = std::function<void(OnvifResult<tds_GetHostnameResponse>)>;

    /**
     * @brief 获取设备基本信息
     *
     * 构建 GetDeviceInformation SOAP 请求 → send_request → 解析响应 → 回调
     */
    void GetDeviceInformation(const tds_GetDeviceInformation& /*req*/, GetDeviceInfoCb cb) {
        std::string body = build_get_device_info_request();
        std::string action = "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation";
        auto client = onvif_client_.lock();     // 尝试升级为 shared_ptr
        if (!client) { cb(std::make_shared<ResultWrapper<tds_GetDeviceInformationResponse>>()); return; }
        client->send_request(
            client->device_url_, action, body,
            [cb](const std::string& xml) {
                auto result = std::make_shared<ResultWrapper<tds_GetDeviceInformationResponse>>();
                parse_get_device_info_response(xml,
                    result->data.Manufacturer, result->data.Model,
                    result->data.FirmwareVersion, result->data.SerialNumber,
                    result->data.HardwareId);
                cb(result);
            });
    }

    /**
     * @brief 获取设备能力集（Media / PTZ 服务地址）
     */
    void GetCapabilities(const tds_GetCapabilities& /*req*/, GetCapabilitiesCb cb) {
        std::string body = build_get_capabilities_request();
        std::string action = "http://www.onvif.org/ver10/device/wsdl/GetCapabilities";
        auto client = onvif_client_.lock();
        if (!client) { cb(std::make_shared<ResultWrapper<tds_GetCapabilitiesResponse>>()); return; }
        client->send_request(
            client->device_url_, action, body,
            [cb](const std::string& xml) {
                auto result = std::make_shared<ResultWrapper<tds_GetCapabilitiesResponse>>();
                std::string mx, px;
                if (parse_get_capabilities_response(xml, mx, px)) {
                    if (!mx.empty()) result->data.Capabilities.Media.XAddr = mx;
                    if (!px.empty()) result->data.Capabilities.PTZ.XAddr = px;
                }
                cb(result);
            });
    }

    /**
     * @brief 获取设备主机名
     */
    void GetHostname(const tds_GetHostname& /*req*/, GetHostnameCb cb) {
        std::string body = build_get_hostname_request();
        std::string action = "http://www.onvif.org/ver10/device/wsdl/GetHostname";
        auto client = onvif_client_.lock();
        if (!client) { cb(std::make_shared<ResultWrapper<tds_GetHostnameResponse>>()); return; }
        client->send_request(
            client->device_url_, action, body,
            [cb](const std::string& xml) {
                auto result = std::make_shared<ResultWrapper<tds_GetHostnameResponse>>();
                std::string hn;
                if (parse_get_hostname_response(xml, hn))
                    result->data.HostnameInformation.Name = hn;
                cb(result);
            });
    }

    std::weak_ptr<OnvifClient> onvif_client_;  // 弱引用，避免循环引用
};

// ============================================================================
// MediaClient
// ============================================================================

/**
 * @brief ONVIF Media Service 客户端
 *
 * 提供媒体配置接口：
 *   GetProfiles      — 获取所有 Profile（第 0 个=主码流，第 1 个=子码流）
 *   GetStreamUri     — 根据 Profile 获取 RTSP 流地址
 *   GetSnapshotUri   — 根据 Profile 获取 JPEG 快照地址
 *
 * MediaClient 持有 media_xaddr_（媒体服务 URL），直接发往 Media 服务。
 */
class MediaClient {
public:
    MediaClient(std::shared_ptr<OnvifClient> client, const std::string& media_xaddr)
        : onvif_client_(client), media_xaddr_(media_xaddr) {}

    using GetProfilesCb    = std::function<void(OnvifResult<trt_GetProfilesResponse>)>;
    using GetStreamUriCb   = std::function<void(OnvifResult<trt_GetStreamUriResponse>)>;
    using GetSnapshotUriCb = std::function<void(OnvifResult<trt_GetSnapshotUriResponse>)>;

    /**
     * @brief 获取所有 Profile（媒体配置列表）
     *
     * 这是拉流前的必要步骤：
     *   GetProfiles → 遍历 Profiles → 取 token → GetStreamUri → FFmpeg 拉流
     */
    void GetProfiles(const trt_GetProfiles& /*req*/, GetProfilesCb cb) {
        std::string body = build_get_profiles_request(media_xaddr_);
        std::string action = "http://www.onvif.org/ver10/media/wsdl/GetProfiles";
        auto client = onvif_client_.lock();
        if (!client) { cb(std::make_shared<ResultWrapper<trt_GetProfilesResponse>>()); return; }
        client->send_request(
            media_xaddr_, action, body,
            [cb](const std::string& xml) {
                auto result = std::make_shared<ResultWrapper<trt_GetProfilesResponse>>();
                parse_get_profiles_response(xml, result->data.Profiles);
                cb(result);
            });
    }

    /**
     * @brief 获取指定 Profile 的 RTSP 流地址
     *
     * @param req  包含 ProfileToken 和 StreamSetup（Unicast/RTSP）
     * @param cb   回调返回 RTSP URI
     */
    void GetStreamUri(const trt_GetStreamUri& req, GetStreamUriCb cb) {
        std::string stream_type = (req.StreamSetup.Stream == tt_StreamType::RTP_Multicast)
                                    ? "RTP-Multicast" : "RTP-Unicast";
        std::string body = build_get_stream_uri_request(media_xaddr_, req.ProfileToken, stream_type);
        std::string action = "http://www.onvif.org/ver10/media/wsdl/GetStreamUri";
        auto client = onvif_client_.lock();
        if (!client) { cb(std::make_shared<ResultWrapper<trt_GetStreamUriResponse>>()); return; }
        client->send_request(
            media_xaddr_, action, body,
            [cb](const std::string& xml) {
                auto result = std::make_shared<ResultWrapper<trt_GetStreamUriResponse>>();
                parse_get_stream_uri_response(xml, result->data.MediaURI.Uri);
                cb(result);
            });
    }

    /**
     * @brief 获取指定 Profile 的 JPEG 快照 URI
     */
    void GetSnapshotUri(const trt_GetSnapshotUri& req, GetSnapshotUriCb cb) {
        std::string body = build_get_snapshot_uri_request(media_xaddr_, req.ProfileToken);
        std::string action = "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri";
        auto client = onvif_client_.lock();
        if (!client) { cb(std::make_shared<ResultWrapper<trt_GetSnapshotUriResponse>>()); return; }
        client->send_request(
            media_xaddr_, action, body,
            [cb](const std::string& xml) {
                auto result = std::make_shared<ResultWrapper<trt_GetSnapshotUriResponse>>();
                parse_get_snapshot_uri_response(xml, result->data.MediaUri.Uri);
                cb(result);
            });
    }

    std::weak_ptr<OnvifClient> onvif_client_;
    std::string media_xaddr_;
};

}  // namespace libonvif_client

// ============================================================================
// WS-Discovery（多播发现，全部内联）
// ============================================================================
//
// WS-Discovery（Web Services Discovery）是 ONVIF 协议的设备发现机制，
// 通过 UDP 多播在局域网内广播发现请求，ONVIF 设备响应后上报自己的服务地址。
//
// 多播地址：239.255.255.250:3702
// 协议：UDP（无需建立连接，发送后等待响应）
// 消息格式：SOAP 1.2（与 HTTP SOAP 相同格式，只是通过 UDP 传输）
//
// ONVIF 规范要求所有 ONVIF 设备必须支持 WS-Discovery。
//
// 完整流程（discover_impl）：
//   1. 创建 UDP socket，加入多播组
//   2. 设置 socket 超时（recvfrom 在超时时返回 -1）
//   3. sendto 发送 Probe 到多播地址
//   4. 循环 recvfrom 接收所有 ProbeMatches 响应
//   5. 过滤含 "ProbeMatches" 的响应，提取 XAddrs，去重后返回

namespace ws_discovery {

/** 多播地址（ONVIF 规范固定为 239.255.255.250） */
const char* MULTICAST_IP = "239.255.255.250";
/** 多播端口（ONVIF 规范固定为 3702） */
const int MULTICAST_PORT = 3702;

static std::string build_probe_msg() {
    // Types 可选；部分固件只对无过滤的 Probe 响应（与 onvif_discovery_client 一致）
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:wsd="http://schemas.xmlsoap.org/ws/2005/04/discovery">
   <soap:Header>
      <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>
      <wsa:MessageID>uuid:onvif-c-api-</wsa:MessageID>
      <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>
   </soap:Header>
   <soap:Body>
      <wsd:Probe/>
   </soap:Body>
</soap:Envelope>)";
}

static std::string extract_tag(const std::string& xml, const std::string& tag) {
    size_t sp = xml.find("<" + tag);
    if (sp == std::string::npos) {
        size_t colon = tag.find(':');
        if (colon != std::string::npos) {
            std::string local = tag.substr(colon + 1);
            sp = xml.find("<" + local);
            if (sp == std::string::npos) return "";
            sp = xml.find(">", sp);
            if (sp == std::string::npos) return "";
            sp++;
            size_t ep = xml.find("</" + local, sp);
            if (ep == std::string::npos) return "";
            return xml.substr(sp, ep - sp);
        }
        return "";
    }
    sp = xml.find(">", sp);
    if (sp == std::string::npos) return "";
    sp++;
    size_t ep = xml.find("</" + tag, sp);
    if (ep == std::string::npos) return "";
    return xml.substr(sp, ep - sp);
}

static std::string extract_tag_ns(const std::string& xml, const std::string& ns_prefix, const std::string& tag) {
    return extract_tag(xml, ns_prefix + ":" + tag);
}

/** ProbeMatches 中 XAddrs 命名空间因厂商而异（wsdd / d / wsd 等）。 */
static std::string extract_probe_xaddrs(const std::string& response) {
    std::string x = extract_tag_ns(response, "wsdd", "XAddrs");
    if (!x.empty()) return x;
    x = extract_tag_ns(response, "d", "XAddrs");
    if (!x.empty()) return x;
    x = extract_tag_ns(response, "wsd", "XAddrs");
    if (!x.empty()) return x;
    return extract_tag(response, "XAddrs");
}

/**
 * WS-Discovery 的 XAddrs 常为 CGI 等非标准路径；若路径中不含 /onvif/device_service，
 * 必须改为标准设备服务 URL，否则 GetServices/GetProfiles 会失败（与 onvif_discovery_client 一致）。
 */
static std::string normalize_to_device_service_url(const std::string& single_url) {
    std::string url = single_url;
    while (!url.empty() && (url.front() == ' ' || url.front() == '\t' || url.front() == '\r' || url.front() == '\n'))
        url.erase(url.begin());
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '\r' || url.back() == '\n'))
        url.pop_back();
    if (url.empty()) return "";

    size_t pos_scheme = url.find("://");
    if (pos_scheme == std::string::npos) return "";

    std::string scheme = url.substr(0, pos_scheme);
    size_t pos_path = url.find('/', pos_scheme + 3);
    std::string host_port;
    std::string path = "/";
    if (pos_path != std::string::npos) {
        host_port = url.substr(pos_scheme + 3, pos_path - (pos_scheme + 3));
        path = url.substr(pos_path);
    } else {
        host_port = url.substr(pos_scheme + 3);
    }

    if (path.find("/onvif/device_service") != std::string::npos)
        return url;

    return scheme + "://" + host_port + "/onvif/device_service";
}

}  // namespace ws_discovery

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 执行 WS-Discovery 多播发现（UDP 多播）
 *
 * 在独立线程中启动（非阻塞，调用后立即返回，实际超时由 recvfrom 控制）。
 * discover_impl() 内部会等待所有响应（或超时）后才返回。
 *
 * 完整流程：
 * 1. socket(AF_INET, SOCK_DGRAM, 0) — 创建 UDP socket
 * 2. setsockopt(SO_RCVTIMEO) — 设置 recv 超时
 * 3. sendto() — 发送 SOAP Probe 到多播地址（与 onvif_discovery_client 相同，不依赖 IP_ADD_MEMBERSHIP；
 *    设备通常以单播回复到本机临时端口）
 * 4. while(true) recvfrom() — 每轮重置 addrlen，循环接收直到超时
 * 5. CLOSE_SOCKET — 关闭 socket
 *
 * @param timeout_ms  recvfrom 超时时间（毫秒）
 * @return            所有发现的设备 XAddrs 列表
 *
 * 注意：多播在部分网络环境下可能不工作（如 Docker 容器、虚拟机、VPN），
 *       此时 discover_impl() 会返回空列表，需要手动指定设备 URL。
 */
static std::vector<std::string> discover_impl(int timeout_ms) {
    std::vector<std::string> out;

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return out;

#ifdef _WIN32
    int tv = timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    struct sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port   = htons(ws_discovery::MULTICAST_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(ws_discovery::MULTICAST_IP);

    std::string probe_msg = ws_discovery::build_probe_msg();
    sendto(sock, probe_msg.c_str(), (int)probe_msg.length(), 0,
           (sockaddr*)&dest_addr, sizeof(dest_addr));

    char buffer[8192];
    struct sockaddr_in from_addr{};
    socklen_t from_len = sizeof(from_addr);
    std::set<std::string> seen;

    while (true) {
        from_len = sizeof(from_addr);
        int recv_len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                (sockaddr*)&from_addr, &from_len);
        if (recv_len <= 0) break;
        buffer[recv_len] = '\0';
        std::string response(buffer);

        if (response.find("ProbeMatches") == std::string::npos) continue;

        std::string xaddrs = ws_discovery::extract_probe_xaddrs(response);
        if (xaddrs.empty()) continue;

        if (seen.insert(xaddrs).second) out.push_back(xaddrs);
    }

    CLOSE_SOCKET(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return out;
}

static std::string extract_ip_from_url(const std::string& url) {
    size_t a = url.find("://");
    if (a == std::string::npos) return url;
    size_t b = url.find('/', a + 3);
    std::string host = url.substr(a + 3, b - (a + 3));
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return host;
}

static libonvif_client::StreamDataType identify_stream_data_type(AVMediaType codec_type, AVCodecID codec_id) {
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        switch (codec_id) {
            case AV_CODEC_ID_H264:   return libonvif_client::StreamDataType::VIDEO_H264;
            case AV_CODEC_ID_HEVC:   return libonvif_client::StreamDataType::VIDEO_H265;
            case AV_CODEC_ID_MJPEG:  return libonvif_client::StreamDataType::VIDEO_JPEG;
            default:                 return libonvif_client::StreamDataType::VIDEO_H264;
        }
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        switch (codec_id) {
            case AV_CODEC_ID_AAC:        return libonvif_client::StreamDataType::AUDIO_AAC;
            case AV_CODEC_ID_PCM_ALAW:  return libonvif_client::StreamDataType::AUDIO_G711A;
            case AV_CODEC_ID_PCM_MULAW: return libonvif_client::StreamDataType::AUDIO_G711U;
            case AV_CODEC_ID_PCM_S16LE: return libonvif_client::StreamDataType::AUDIO_PCM;
            default:                    return libonvif_client::StreamDataType::AUDIO_AAC;
        }
    }
    return libonvif_client::StreamDataType::UNKNOWN;
}

static onvif_codec_e to_onvif_codec(libonvif_client::StreamDataType dt) {
    switch (dt) {
        case libonvif_client::StreamDataType::VIDEO_H264:   return ONVIF_CODEC_H264;
        case libonvif_client::StreamDataType::VIDEO_H265:   return ONVIF_CODEC_H265;
        case libonvif_client::StreamDataType::VIDEO_JPEG:   return ONVIF_CODEC_JPEG;
        case libonvif_client::StreamDataType::AUDIO_AAC:    return ONVIF_CODEC_AAC;
        case libonvif_client::StreamDataType::AUDIO_G711U:  return ONVIF_CODEC_G711U;
        case libonvif_client::StreamDataType::AUDIO_G711A:  return ONVIF_CODEC_G711A;
        default:                                             return ONVIF_CODEC_H264;
    }
}

static std::string* select_stream_uri(onvif_stream_type_e stype,
                                       libonvif_client::OnvifDeviceInfo& info) {
    if (stype == ONVIF_STREAM_TYPE_SUB) return &info.sub_stream_uri;
    return &info.main_stream_uri;
}

static size_t snapshot_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    auto* buf = static_cast<std::vector<uint8_t>*>(userp);
    const uint8_t* c = static_cast<const uint8_t*>(contents);
    buf->insert(buf->end(), c, c + realsize);
    return realsize;
}

static onvif_error_e fetch_snapshot(const std::string& snapshot_url,
                                     const std::string& username,
                                     const std::string& password,
                                     std::vector<uint8_t>& img_out) {
    if (snapshot_url.empty()) return ONVIF_ERR_NOT_FOUND;

    CURL* curl = curl_easy_init();
    if (!curl) return ONVIF_ERR_NETWORK;

    img_out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, snapshot_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, snapshot_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &img_out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    if (!username.empty()) {
        std::string userpwd = username + ":" + password;
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return ONVIF_ERR_NETWORK;
    if (http_code == 401 || http_code == 403) return ONVIF_ERR_AUTH;
    if (http_code >= 400 || img_out.empty()) return ONVIF_ERR_DEVICE;
    return ONVIF_OK;
}

// ============================================================================
// StreamContext（FFmpeg RTSP 拉流上下文）
// ============================================================================
//
// 负责管理一条 RTSP 流的全生命周期。
//
// 设计动机：
//   多设备 × 每设备主/子码流 → 需要 N×2 个独立拉流上下文
//   每个上下文包含：
//     - FFmpeg 流信息（video_index、audio_index）
//     - 时间戳基准（video_base_pts_ms、audio_base_pts_ms）
//     - 回调（user_cb）
//     - 线程控制（running / stop_flag）
//
// 并发控制：
//   - running（atomic<bool>）  → 防止 Pull() 被重复调用
//   - stop_flag（atomic<int>） → 1=请求停止，0=继续读包
//     - Pull() 启动前 store(0)；Stop() store(1) 通知拉流线程退出
//     - PullImpl() 中 while(stop_flag != 1) 读帧循环

/**
 * @brief 单路拉流上下文
 *
 * 每设备每码流各有一个实例，互不干扰。
 *
 * 成员说明：
 *   running / stop_flag  — 线程安全标志位
 *   video_index / audio_index — FFmpeg streams[] 中的索引
 *   video_base_pts_ms — 视频首帧 PTS（建立相对时间基准）
 *   audio_base_pts_ms — 音频首帧 PTS
 *   user_cb / user_data — 上层回调（每帧触发一次）
 *   device_index — 所属设备在 g_devices 中的索引
 */
struct StreamContext {
    std::atomic<bool> running{false};   // 当前是否处于运行状态
    std::shared_ptr<std::thread> thread; // 拉流线程
    int video_index = -1;              // FFmpeg 视频流索引
    int audio_index = -1;              // FFmpeg 音频流索引
    int video_width = 0;               // 视频分辨率宽（像素）
    int video_height = 0;              // 视频分辨率高（像素）
    uint64_t video_base_pts_ms = 0;   // 视频首帧 PTS（毫秒）
    uint64_t audio_base_pts_ms = 0;   // 音频首帧 PTS（毫秒）
    bool video_first = false;         // 是否已收到首帧视频
    bool audio_first = false;         // 是否已收到首帧音频
    std::atomic<int> stop_flag{1};    // 1=已请求停止（空闲），0=拉流中

    media_stream_callback user_cb = nullptr;  // 上层音视频帧回调
    void* user_data = nullptr;       // 用户自定义数据（透传给 user_cb）
    int device_index = -1;          // 所属设备在 g_devices 中的索引

    void Pull(const std::string& rtsp_url,
              const std::string& /*username*/,
              const std::string& /*password*/) {
        if (running.load(std::memory_order_acquire)) return;
        running.store(true, std::memory_order_release);
        stop_flag.store(0, std::memory_order_release);  // 0：进入读包循环
        video_first = false;
        audio_first = false;
        video_base_pts_ms = 0;
        audio_base_pts_ms = 0;

        thread = std::make_shared<std::thread>([this, rtsp_url]() {
            PullImpl(rtsp_url);
        });
    }

    void Stop() {
        stop_flag.store(1, std::memory_order_release);  // 1：请求退出读循环
        if (thread && thread->joinable()) thread->join();
        thread.reset();
        running.store(false, std::memory_order_release);
    }

private:
    void PullImpl(const std::string& rtsp_url) {
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "rtsp_transport",    "tcp",         0);
        av_dict_set(&opts, "stimeout",           "10000000",    0);
        av_dict_set(&opts, "max_delay",          "500000",      0);
        av_dict_set(&opts, "buffer_size",        "1024000",     0);
        av_dict_set(&opts, "reconnect",          "1",           0);
        av_dict_set(&opts, "reconnect_streamed", "1",           0);
        av_dict_set(&opts, "reconnect_delay_max","5",           0);

        AVFormatContext* ctx = nullptr;
        int ret = avformat_open_input(&ctx, rtsp_url.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            running.store(false, std::memory_order_release);
            return;
        }

        if (avformat_find_stream_info(ctx, nullptr) < 0) {
            avformat_close_input(&ctx);
            running.store(false, std::memory_order_release);
            return;
        }

        video_index = -1;
        audio_index = -1;
        for (unsigned i = 0; i < ctx->nb_streams; ++i) {
            AVStream* st = ctx->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0) {
                video_index = (int)i;
                video_width  = st->codecpar->width;
                video_height = st->codecpar->height;
            }
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
                audio_index = (int)i;
            }
        }

        if (video_index < 0) {
            avformat_close_input(&ctx);
            running.store(false, std::memory_order_release);
            return;
        }

        AVPacket* pkt = av_packet_alloc();
        uint64_t last_vpts = 0;
        uint64_t last_apts = 0;

        while (stop_flag.load(std::memory_order_acquire) != 1) {
            ret = av_read_frame(ctx, pkt);
            if (ret < 0) {
                if (stop_flag.load(std::memory_order_acquire) == 1) break;
                av_usleep(100000);
                continue;
            }

            if (pkt->stream_index == video_index && pkt->size > 0) {
                AVStream* vs = ctx->streams[video_index];
                uint64_t ptsMs = 0;
                if (pkt->pts != AV_NOPTS_VALUE)
                    ptsMs = pkt->pts * 1000LL * vs->time_base.num / vs->time_base.den;

                if (!video_first) {
                    video_base_pts_ms = ptsMs;
                    video_first = true;
                    last_vpts = 0;
                } else {
                    if (ptsMs >= video_base_pts_ms) ptsMs = ptsMs - video_base_pts_ms;
                    if (ptsMs < last_vpts) ptsMs = last_vpts;
                    last_vpts = ptsMs;
                }

                libonvif_client::StreamDataType dt =
                    identify_stream_data_type(AVMEDIA_TYPE_VIDEO, vs->codecpar->codec_id);
                if (user_cb)
                    user_cb(device_index, pkt->data, static_cast<uint32_t>(pkt->size),
                            to_onvif_codec(dt), user_data);
            }
            else if (pkt->stream_index == audio_index && pkt->size > 0) {
                AVStream* as = ctx->streams[audio_index];
                uint64_t ptsMs = 0;
                if (pkt->pts != AV_NOPTS_VALUE)
                    ptsMs = pkt->pts * 1000LL * as->time_base.num / as->time_base.den;

                if (!audio_first) {
                    audio_base_pts_ms = ptsMs;
                    audio_first = true;
                    last_apts = 0;
                } else {
                    if (ptsMs >= audio_base_pts_ms) ptsMs = ptsMs - audio_base_pts_ms;
                    if (ptsMs < last_apts) ptsMs = last_apts;
                    last_apts = ptsMs;
                }

                libonvif_client::StreamDataType dt =
                    identify_stream_data_type(AVMEDIA_TYPE_AUDIO, as->codecpar->codec_id);
                if (user_cb)
                    user_cb(device_index, pkt->data, static_cast<uint32_t>(pkt->size),
                            to_onvif_codec(dt), user_data);
            }

            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        avformat_close_input(&ctx);
        running.store(false, std::memory_order_release);
    }
};

// ============================================================================
// DeviceContext
// ============================================================================

struct DeviceContext {
    libonvif_client::OnvifDeviceInfo device_info;
    StreamContext main_stream;
    StreamContext sub_stream;
    std::string username = "admin";
    std::string password = "admin123";
};

// ============================================================================
// 全局变量
// ============================================================================

static std::atomic<bool> g_ffmpeg_inited{false};
static std::atomic<bool> g_curl_inited{false};
static std::mutex g_dev_mutex;
static std::vector<std::shared_ptr<DeviceContext>> g_devices;

/** 将内部视频编码枚举转为短标签，便于调试日志阅读 */
static const char* video_codec_label(libonvif_client::StreamDataType t) {
    switch (t) {
        case libonvif_client::StreamDataType::VIDEO_H264: return "H264";
        case libonvif_client::StreamDataType::VIDEO_H265: return "H265";
        case libonvif_client::StreamDataType::VIDEO_JPEG: return "MJPEG";
        default: return "?";
    }
}

/**
 * 在 ONVIF 探测完成后打印设备详情（不写密码）。
 * 便于确认子码流 RTSP、主/子抓拍 URL、厂商信息等是否已从设备取回。
 */
static void log_onvif_device_fetched(const libonvif_client::OnvifDeviceInfo& i) {
    std::cerr << "[onvif] ---------- 设备信息（ONVIF 探测完成）----------\n";
    std::cerr << "[onvif] device_service: " << i.device_service_url << '\n';
    std::cerr << "[onvif] 认证用户: " << i.username << "（密码已省略）\n";
    std::cerr << "[onvif] 厂商=" << i.manufacturer << " 型号=" << i.model
              << " 固件=" << i.firmware_version << '\n';
    std::cerr << "[onvif] 序列号=" << i.serial_number << " 硬件ID=" << i.hardware_id
              << " 主机名=" << i.hostname << '\n';
    std::cerr << "[onvif] Media XAddr=" << i.media_xaddr << '\n';
    std::cerr << "[onvif] PTZ XAddr=" << i.ptz_xaddr << '\n';
    std::cerr << "[onvif] 主 ProfileToken=" << i.profile_token
              << " 视频编码=" << video_codec_label(i.main_video_codec) << '\n';
    std::cerr << "[onvif] 主码流 RTSP: " << i.main_stream_uri << '\n';
    std::cerr << "[onvif] 子码流 RTSP: "
              << (i.sub_stream_uri.empty() ? "(无/未配置第二路 Profile)" : i.sub_stream_uri) << '\n';
    std::cerr << "[onvif] 子码流 视频编码=" << video_codec_label(i.sub_video_codec) << '\n';
    std::cerr << "[onvif] 主抓拍 URL: "
              << (i.main_snapshot_uri.empty() ? "(无)" : i.main_snapshot_uri) << '\n';
    std::cerr << "[onvif] 子抓拍 URL: "
              << (i.sub_snapshot_uri.empty() ? "(无)" : i.sub_snapshot_uri) << '\n';
    std::cerr << "[onvif] --------------------------------------------\n";
}

// ============================================================================
// fetch_device_info（完整 ONVIF 认证流程）
// ============================================================================
//
// 依次调用 9 个 ONVIF SOAP 接口，填充 OnvifDeviceInfo。
// 所有调用均为异步回调，通过 condition_variable 同步等待。
//
// 调用顺序：
//   1. initialize()              — GetServices，建立会话
//   2. GetDeviceInformation      — 厂商/型号/固件/序列号/硬件ID
//   3. GetCapabilities         — Media / PTZ 服务地址
//   4. GetHostname              — 设备主机名
//   5. create_service<Media>()  — 获取 Media 服务客户端
//   6. GetProfiles              — 所有 Profile（第0=主，第1=子）
//   7. GetStreamUri (主)        — 主码流 RTSP URI
//   8. GetStreamUri (子)        — 子码流 RTSP URI
//   9. GetSnapshotUri (主)      — 主码流 JPEG 快照 URI
//   10. GetSnapshotUri (子)     — 子码流 JPEG 快照 URI
//
// 同步机制：
//   make_sync(done) → 在回调中调用 setter(done) → notify_one()
//   make_wait(done) → unique_lock + wait_for(10s) → 阻塞等待
//   任意接口超时（10s） → 直接返回 false

/**
 * @brief 获取指定设备的完整 ONVIF 信息
 *
 * @param device_url   设备服务 URL
 * @param username     认证用户名
 * @param password     认证密码
 * @param out_info    输出：设备完整信息
 * @return            true 获取成功，false 失败（超时或认证失败）
 */
static bool fetch_device_info(const std::string& device_url,
                              const std::string& username,
                              const std::string& password,
                              libonvif_client::OnvifDeviceInfo& out_info) {
    auto http_client = std::make_shared<libonvif_client::CurlHttpClient>(true, 2);
    http_client->set_ssl_verify(false);
    http_client->set_connect_timeout(10);

    auto onvif_client = std::make_shared<libonvif_client::OnvifClient>(
        device_url, username, password, http_client);

    std::mutex cb_mutex;
    std::condition_variable cb_cv;

    auto make_sync = [&](bool& flag) {
        return [&](bool&) {
            std::lock_guard<std::mutex> l(cb_mutex);
            flag = true;
            cb_cv.notify_one();
        };
    };

    auto make_wait = [&](bool& flag, int timeout_sec) -> bool {
        std::unique_lock<std::mutex> l(cb_mutex);
        return cb_cv.wait_for(l, std::chrono::seconds(timeout_sec), [&]{ return flag; });
    };

    // Initialize
    {
        bool ok = false;
        bool done = false;
        auto setter = make_sync(done);
        onvif_client->initialize(
            [&](libonvif_client::OnvifResult<libonvif_client::tds_GetServicesResponse>&& result) {
                if (!result->is_error()) ok = true;
                setter(done);
            });
        if (!make_wait(done, 10) || !ok) {
            return false;
        }
    }

    auto device_client = onvif_client->create_device_client();

    // GetDeviceInformation
    {
        bool done = false;
        libonvif_client::tds_GetDeviceInformationResponse dev_info;
        auto setter = make_sync(done);
        libonvif_client::tds_GetDeviceInformation req;
        device_client->GetDeviceInformation(req,
            [&](libonvif_client::OnvifResult<libonvif_client::tds_GetDeviceInformationResponse>&& result) {
                if (!result->is_error()) dev_info = result->data;
                setter(done);
            });
        if (make_wait(done, 10)) {
            out_info.manufacturer     = dev_info.Manufacturer;
            out_info.model            = dev_info.Model;
            out_info.firmware_version = dev_info.FirmwareVersion;
            out_info.serial_number   = dev_info.SerialNumber;
            out_info.hardware_id      = dev_info.HardwareId;
        }
    }

    // GetCapabilities
    {
        bool done = false;
        auto setter = make_sync(done);
        libonvif_client::tds_GetCapabilities req;
        device_client->GetCapabilities(req,
            [&](libonvif_client::OnvifResult<libonvif_client::tds_GetCapabilitiesResponse>&& result) {
                if (!result->is_error()) {
                    if (!result->data.Capabilities.Media.XAddr.empty())
                        out_info.media_xaddr = result->data.Capabilities.Media.XAddr;
                    if (!result->data.Capabilities.PTZ.XAddr.empty())
                        out_info.ptz_xaddr = result->data.Capabilities.PTZ.XAddr;
                }
                setter(done);
            });
        make_wait(done, 10);
    }

    // GetHostname
    {
        bool done = false;
        std::string hostname_val;
        auto setter = make_sync(done);
        libonvif_client::tds_GetHostname req;
        device_client->GetHostname(req,
            [&](libonvif_client::OnvifResult<libonvif_client::tds_GetHostnameResponse>&& result) {
                if (!result->is_error() && !result->data.HostnameInformation.Name.empty())
                    hostname_val = result->data.HostnameInformation.Name;
                setter(done);
            });
        if (make_wait(done, 10)) out_info.hostname = hostname_val;
    }

    // Media service
    auto media_client = onvif_client->create_service<libonvif_client::MediaClient>();
    if (!media_client) {
        return false;
    }

    // GetProfiles
    bool prof_done = false;
    std::vector<libonvif_client::tt_Profile> profiles;
    auto setter = make_sync(prof_done);
    libonvif_client::trt_GetProfiles req;
    media_client->GetProfiles(req,
        [&](libonvif_client::OnvifResult<libonvif_client::trt_GetProfilesResponse>&& result) {
            if (!result->is_error()) profiles = result->data.Profiles;
            setter(prof_done);
        });
    if (!make_wait(prof_done, 10) || profiles.empty()) {
        return false;
    }

    std::string profile_token;
    std::string sub_profile_token;
    libonvif_client::StreamDataType main_codec = libonvif_client::StreamDataType::VIDEO_H264;
    libonvif_client::StreamDataType sub_codec   = libonvif_client::StreamDataType::VIDEO_H264;

    for (size_t i = 0; i < profiles.size(); ++i) {
        const auto& prof = profiles[i];
        if (i == 0) {
            profile_token = prof.token;
            if (!prof.VideoEncoderConfiguration.Name.empty()) {
                if (prof.VideoEncoderConfiguration.Encoding == libonvif_client::tt_VideoEncoding::JPEG)
                    main_codec = libonvif_client::StreamDataType::VIDEO_JPEG;
            }
        } else if (i == 1) {
            sub_profile_token = prof.token;
            if (!prof.VideoEncoderConfiguration.Name.empty()) {
                if (prof.VideoEncoderConfiguration.Encoding == libonvif_client::tt_VideoEncoding::JPEG)
                    sub_codec = libonvif_client::StreamDataType::VIDEO_JPEG;
            }
        }
    }

    out_info.main_video_codec = main_codec;
    out_info.sub_video_codec  = sub_codec;

    // GetStreamUri — main
    {
        bool done = false;
        std::string main_stream_uri;
        auto setter2 = make_sync(done);
        libonvif_client::trt_GetStreamUri uri_req;
        uri_req.ProfileToken = profile_token;
        uri_req.StreamSetup.Stream = libonvif_client::tt_StreamType::RTP_Unicast;
        uri_req.StreamSetup.Transport.Protocol = libonvif_client::tt_TransportProtocol::RTSP;
        media_client->GetStreamUri(uri_req,
            [&](libonvif_client::OnvifResult<libonvif_client::trt_GetStreamUriResponse>&& result) {
                if (!result->is_error()) main_stream_uri = result->data.MediaURI.Uri;
                setter2(done);
            });
        if (make_wait(done, 10)) out_info.main_stream_uri = main_stream_uri;
    }

    // GetStreamUri — sub
    if (!sub_profile_token.empty()) {
        bool done = false;
        std::string sub_stream_uri;
        auto setter2 = make_sync(done);
        libonvif_client::trt_GetStreamUri uri_req;
        uri_req.ProfileToken = sub_profile_token;
        uri_req.StreamSetup.Stream = libonvif_client::tt_StreamType::RTP_Unicast;
        uri_req.StreamSetup.Transport.Protocol = libonvif_client::tt_TransportProtocol::RTSP;
        media_client->GetStreamUri(uri_req,
            [&](libonvif_client::OnvifResult<libonvif_client::trt_GetStreamUriResponse>&& result) {
                if (!result->is_error()) sub_stream_uri = result->data.MediaURI.Uri;
                setter2(done);
            });
        if (make_wait(done, 10)) out_info.sub_stream_uri = sub_stream_uri;
    }

    // GetSnapshotUri — main
    {
        bool done = false;
        std::string main_snap_uri;
        auto setter2 = make_sync(done);
        libonvif_client::trt_GetSnapshotUri snap_req;
        snap_req.ProfileToken = profile_token;
        media_client->GetSnapshotUri(snap_req,
            [&](libonvif_client::OnvifResult<libonvif_client::trt_GetSnapshotUriResponse>&& result) {
                if (!result->is_error()) main_snap_uri = result->data.MediaUri.Uri;
                setter2(done);
            });
        if (make_wait(done, 10)) out_info.main_snapshot_uri = main_snap_uri;
    }

    // GetSnapshotUri — sub
    if (!sub_profile_token.empty()) {
        bool done = false;
        std::string sub_snap_uri;
        auto setter2 = make_sync(done);
        libonvif_client::trt_GetSnapshotUri snap_req;
        snap_req.ProfileToken = sub_profile_token;
        media_client->GetSnapshotUri(snap_req,
            [&](libonvif_client::OnvifResult<libonvif_client::trt_GetSnapshotUriResponse>&& result) {
                if (!result->is_error()) sub_snap_uri = result->data.MediaUri.Uri;
                setter2(done);
            });
        if (make_wait(done, 10)) out_info.sub_snapshot_uri = sub_snap_uri;
    }

    out_info.device_service_url = device_url;
    out_info.username           = username;
    out_info.password           = password;
    out_info.profile_token      = profile_token;

    log_onvif_device_fetched(out_info);
    return true;
}

// ============================================================================
// 对外 C API 实现（onvif.h 中声明的所有函数）
// ============================================================================
//
// API 列表：
//   mg_onvif_client_init           — 初始化（FFmpeg + libcurl）
//   mg_onvif_client_deinit         — 释放资源
//   mg_onvif_discover_devices      — WS-Discovery + ONVIF 认证
//   mg_onvif_set_device_auth       — 修改设备认证凭据
//   mg_onvif_start_media_stream     — 启动主/子码流拉流
//   mg_onvif_stop_media_stream     — 停止拉流
//   mg_onvif_capture_image          — HTTP GET 抓拍 JPEG
//
// 线程安全：
//   g_devices 全局设备列表通过 std::mutex g_dev_mutex 保护。
//   所有涉及 g_devices 读写操作的 API 都在锁内完成。

/**
 * @brief 初始化 ONVIF 客户端
 *
 * 调用 ONVIF SDK 内部组件的初始化（全局一次性）：
 *   avformat_network_init()   → FFmpeg 网络组件（RTSP/网络流支持）
 *   curl_global_init()        → libcurl 全局资源
 *
 * 使用 atomic 检查保证只初始化一次，重复调用安全。
 * 应与 mg_onvif_client_deinit() 配对使用。
 *
 * @return  错误码（始终返回 ONVIF_OK）
 */
onvif_error_e mg_onvif_client_init(void) {
    if (!g_ffmpeg_inited.load(std::memory_order_acquire)) {
        avformat_network_init();
        g_ffmpeg_inited.store(true, std::memory_order_release);
    }
    if (!g_curl_inited.load(std::memory_order_acquire)) {
        curl_global_init(CURL_GLOBAL_ALL);
        g_curl_inited.store(true, std::memory_order_release);
    }
    return ONVIF_OK;
}

/**
 * @brief 释放 ONVIF 客户端资源
 *
 * 释放顺序（必须严格遵守）：
 * 1. 锁住 g_dev_mutex → 停止所有设备的 main_stream 和 sub_stream
 * 2. 清空 g_devices 列表
 * 3. 释放 FFmpeg 网络资源
 * 4. 释放 libcurl 全局资源
 *
 * 注意：必须在所有拉流停止后再释放 FFmpeg/libcurl，否则可能导致数据竞争。
 */
void mg_onvif_client_deinit(void) {
    {
        std::lock_guard<std::mutex> l(g_dev_mutex);
        for (auto& dev : g_devices) {
            dev->main_stream.Stop();
            dev->sub_stream.Stop();
        }
        g_devices.clear();
    }
    if (g_ffmpeg_inited.load()) {
        avformat_network_deinit();
        g_ffmpeg_inited.store(false);
    }
    if (g_curl_inited.load()) {
        curl_global_cleanup();
        g_curl_inited.store(false);
    }
}

/**
 * @brief 发现局域网内所有 ONVIF 设备
 *
 * 完整流程：
 * 1. discover_impl(timeout_ms)   → WS-Discovery UDP 多播发现
 * 2. 规范化所有 XAddrs URL         → /onvif/device_service 格式
 * 3. 依次尝每个 URL → fetch_device_info() → 填充设备信息
 * 4. 按 IP 升序排序
 * 5. 写入 g_devices 列表
 *
 * 注意：
 *   - 所有设备串行探测，超时的 URL 会被跳过
 *   - 默认凭据 admin/admin123，若失败则该设备不会被加入列表
 *   - g_devices.clear() 会停止旧设备的所有拉流
 *
 * @param count       输入/输出：最大支持数量 / 实际发现数量
 * @param timeout_ms  WS-Discovery 超时（毫秒）
 * @return            错误码
 */
onvif_error_e mg_onvif_discover_devices(int* count, int timeout_ms) {
    if (!count || *count <= 0) return ONVIF_ERR_INVALID_PARAM;

    auto raw_xaddrs = discover_impl(timeout_ms);

    std::vector<std::string> candidates;
    std::set<std::string> seen;
    for (const auto& raw : raw_xaddrs) {
        std::istringstream iss(raw);
        std::string token;
        while (iss >> token) {
            std::string n = ws_discovery::normalize_to_device_service_url(token);
            if (!n.empty() && seen.insert(n).second)
                candidates.push_back(n);
        }
    }

    if (candidates.empty()) {
        *count = 0;
        return ONVIF_OK;
    }

    std::string default_user = "admin";
    std::string default_pass = "admin123";
    std::vector<libonvif_client::OnvifDeviceInfo> discovered;

    for (const auto& url : candidates) {
        if ((int)discovered.size() >= *count) break;
        libonvif_client::OnvifDeviceInfo info;
        if (fetch_device_info(url, default_user, default_pass, info)) {
            discovered.push_back(info);
        }
    }

    if (discovered.empty() && !candidates.empty()) {
        std::cerr << "[onvif] WS-Discovery 得到 " << candidates.size()
                  << " 个候选 URL，但 ONVIF 探测均未成功（默认账号 admin/admin123；"
                  << "或设备路径/网络异常）。可用 mg_onvif_set_device_auth 在发现后改凭据再试。\n";
    }

    std::sort(discovered.begin(), discovered.end(),
        [](const libonvif_client::OnvifDeviceInfo& a,
           const libonvif_client::OnvifDeviceInfo& b) {
            return extract_ip_from_url(a.device_service_url)
                 < extract_ip_from_url(b.device_service_url);
        });

    {
        std::lock_guard<std::mutex> l(g_dev_mutex);
        g_devices.clear();
        for (auto& info : discovered) {
            auto ctx = std::make_shared<DeviceContext>();
            ctx->device_info = info;
            ctx->username = info.username.empty() ? default_user : info.username;
            ctx->password = info.password.empty() ? default_pass : info.password;
            ctx->main_stream.device_index = (int)g_devices.size();
            ctx->sub_stream.device_index  = (int)g_devices.size();
            g_devices.push_back(ctx);
        }
    }

    *count = (int)discovered.size();
    std::cerr << "[onvif] 发现 " << *count << " 台设备（按 IP 升序；每台详情已在探测时打印）\n";
    return ONVIF_OK;
}

/**
 * @brief 设置指定设备的认证凭据
 *
 * 修改 g_devices[index] 中存储的 username / password。
 * 认证信息在拉流（FFmpeg 暂不使用）和抓拍（fetch_snapshot）时生效。
 *
 * @param index    设备索引（0-based）
 * @param username 用户名
 * @param password 密码
 * @return         错误码（ONVIF_ERR_OUT_OF_BOUNDS 索引越界）
 */
onvif_error_e mg_onvif_set_device_auth(int index, const char* username, const char* password) {
    std::lock_guard<std::mutex> l(g_dev_mutex);
    if (index < 0 || index >= (int)g_devices.size()) return ONVIF_ERR_OUT_OF_BOUNDS;
    if (!username || !password) return ONVIF_ERR_INVALID_PARAM;

    g_devices[index]->username = username;
    g_devices[index]->password = password;
    g_devices[index]->device_info.username = username;
    g_devices[index]->device_info.password = password;
    return ONVIF_OK;
}

/**
 * @brief 启动指定设备的指定码流拉取
 *
 * 内部流程：
 * 1. 参数校验（stream_cb 不能为空）
 * 2. 加锁，获取 DeviceContext 和对应 StreamContext
 * 3. select_stream_uri() 根据主/子码流获取对应 RTSP URI
 * 4. URI 为空 → 返回 NOT_FOUND（该码流不支持）
 * 5. 绑定 user_cb 和 user_data
 * 6. 调用 sc->Pull() 启动拉流线程
 *
 * 支持主/子码流同时拉取（各自独立的 StreamContext）。
 *
 * @param index        设备索引（0-based）
 * @param stream_type  码流类型（ONVIF_STREAM_TYPE_MAIN / ONVIF_STREAM_TYPE_SUB）
 * @param stream_cb    音视频帧回调（每帧触发一次）
 * @param user_data   用户自定义数据
 * @return             错误码
 */
onvif_error_e mg_onvif_start_media_stream(int index,
                                            onvif_stream_type_e stream_type,
                                            media_stream_callback stream_cb,
                                            void* user_data) {
    if (!stream_cb) return ONVIF_ERR_INVALID_PARAM;

    std::shared_ptr<DeviceContext> dev;
    StreamContext* sc = nullptr;
    std::string* uri_ptr = nullptr;

    {
        std::lock_guard<std::mutex> l(g_dev_mutex);
        if (index < 0 || index >= (int)g_devices.size()) return ONVIF_ERR_OUT_OF_BOUNDS;
        dev = g_devices[index];
        sc = (stream_type == ONVIF_STREAM_TYPE_SUB) ? &dev->sub_stream : &dev->main_stream;
        uri_ptr = select_stream_uri(stream_type, dev->device_info);
    }

    if (uri_ptr->empty()) return ONVIF_ERR_NOT_FOUND;

    const char* st = (stream_type == ONVIF_STREAM_TYPE_SUB) ? "子码流" : "主码流";
    std::cerr << "[onvif] 设备 #" << index << " " << st << " 拉流已启动: " << *uri_ptr << '\n';

    sc->user_cb = stream_cb;
    sc->user_data = user_data;
    sc->Pull(*uri_ptr, dev->username, dev->password);
    return ONVIF_OK;
}

onvif_error_e mg_onvif_stop_media_stream(int index) {
    std::lock_guard<std::mutex> l(g_dev_mutex);
    if (index < 0 || index >= (int)g_devices.size()) return ONVIF_ERR_OUT_OF_BOUNDS;
    g_devices[index]->main_stream.Stop();
    g_devices[index]->sub_stream.Stop();
    return ONVIF_OK;
}

onvif_error_e mg_onvif_capture_image(int index,
                                      onvif_stream_type_e stream_type,
                                      capture_image_callback img_cb,
                                      void* user_data) {
    if (!img_cb) return ONVIF_ERR_INVALID_PARAM;

    std::string snapshot_url;
    std::string username;
    std::string password;

    {
        std::lock_guard<std::mutex> l(g_dev_mutex);
        if (index < 0 || index >= (int)g_devices.size()) return ONVIF_ERR_OUT_OF_BOUNDS;
        const auto& dev = g_devices[index];
        snapshot_url = (stream_type == ONVIF_STREAM_TYPE_SUB)
                            ? dev->device_info.sub_snapshot_uri
                            : dev->device_info.main_snapshot_uri;
        username = dev->username;
        password = dev->password;
    }

    if (snapshot_url.empty()) return ONVIF_ERR_NOT_FOUND;

    const char* cap = (stream_type == ONVIF_STREAM_TYPE_SUB) ? "子码流" : "主码流";
    std::cerr << "[onvif] 设备 #" << index << " " << cap << " 抓拍: " << snapshot_url << '\n';

    std::vector<uint8_t> img_data;
    onvif_error_e err = fetch_snapshot(snapshot_url, username, password, img_data);
    if (err != ONVIF_OK) return err;

    std::cerr << "[onvif] 设备 #" << index << " " << cap
              << " 抓拍完成, JPEG 字节数=" << img_data.size() << '\n';

    img_cb(index, img_data.data(), static_cast<uint32_t>(img_data.size()),
           ONVIF_CODEC_JPEG, user_data);
    return ONVIF_OK;
}

// ============================================================================
// 演示 / 测试入口
// ============================================================================
// main 函数已迁移到 onvif_demo.cpp，作为独立测试程序使用。
// ============================================================================
