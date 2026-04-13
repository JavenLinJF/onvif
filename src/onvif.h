#ifndef MG_ONVIF_CLIENT_H
#define MG_ONVIF_CLIENT_H

#include <stdint.h>
#include <stdio.h>

/**
 * @brief 错误码枚举
 */
typedef enum {
    ONVIF_OK = 0,                   // 成功
    ONVIF_ERR_INVALID_PARAM = -1,   // 无效参数
    ONVIF_ERR_MEMORY = -2,          // 内存分配失败
    ONVIF_ERR_NETWORK = -3,         // 网络错误 
    ONVIF_ERR_TIMEOUT = -4,         // 操作超时
    ONVIF_ERR_AUTH = -5,            // 认证失败
    ONVIF_ERR_NOT_FOUND = -6,       // 未找到资源
    ONVIF_ERR_NOT_SUPPORTED = -7,   // 不支持的操作
    ONVIF_ERR_DEVICE = -8,          // 设备错误 
    ONVIF_ERR_OUT_OF_BOUNDS = -9,   // 索引越界
    ONVIF_ERR_UNKNOWN = -99         // 未知错误
} onvif_error_e;

/**
 * @brief 编码格式枚举
 */
typedef enum {
    ONVIF_CODEC_H264 = 0,
    ONVIF_CODEC_H265,
    ONVIF_CODEC_G711A,
    ONVIF_CODEC_G711U,
    ONVIF_CODEC_AAC,
    ONVIF_CODEC_JPEG
} onvif_codec_e;

/**
 * @brief 码流类型枚举
 */
typedef enum {
    ONVIF_STREAM_TYPE_MAIN = 0,         // 主码流（高分辨率、高码率）
    ONVIF_STREAM_TYPE_SUB               // 子码流（低分辨率、低码率）
} onvif_stream_type_e;

/**
 * @brief 媒体流数据回调函数类型
 * @param index ONVIF设备列表的索引，索引从 0 开始
 * @param data 流数据（H264/H265/G711A/AAC裸数据）
 * @param len 数据长度
 * @param onvif_codec 媒体流数据格式
 * @param user_data 用户自定义数据
 */
typedef void (*media_stream_callback)(int index, const uint8_t* data, uint32_t len, onvif_codec_e onvif_codec, void* user_data);

/**
 * @brief 抓图数据回调函数类型
 * @param index ONVIF设备列表的索引，索引从 0 开始
 * @param img_data 图片数据
 * @param img_len 图片长度
 * @param onvif_codec 图片格式
 * @param user_data 用户自定义数据
 */
typedef void (*capture_image_callback)(int index, const uint8_t* img_data, uint32_t img_len, onvif_codec_e onvif_codec, void* user_data);

/**
 * @brief 初始化ONVIF客户端
 * @return 错误码
 */
onvif_error_e mg_onvif_client_init(void);

/**
 * @brief 释放ONVIF客户端资源
 */
void mg_onvif_client_deinit(void);

/**
 * @brief 发现局域网内所有ONVIF设备，获取设备详情，获取设备媒体能力，设备列表按照IP升序排序
 * @param count 输入：最大支持的设备数量；输出：实际设备数量
 * @param timeout_ms 超时时间
 * @return 错误码
 */
onvif_error_e mg_onvif_discover_devices(int* count, int timeout_ms);

/**
 * @brief 设置设备登录信息（部分设备需要认证）
 * @param index ONVIF设备列表的索引，索引从 0 开始
 * @param username 用户名
 * @param password 密码
 * @return 错误码
 */
onvif_error_e mg_onvif_set_device_auth(int index, const char* username, const char* password);

/**
 * @brief 启动指定设备的指定码流拉取（核心升级：支持主/子码流）
 * @param index ONVIF设备列表的索引，索引从 0 开始
 * @param stream_type 码流类型
 * @param stream_cb 流数据回调函数
 * @param user_data 用户自定义数据
 * @return 错误码
 */
onvif_error_e mg_onvif_start_media_stream(int index, onvif_stream_type_e stream_type, media_stream_callback stream_cb, void* user_data);

/**
 * @brief 停止媒体流拉取
 * @param index ONVIF设备列表的索引，索引从 0 开始
 * @return 错误码
 */
onvif_error_e mg_onvif_stop_media_stream(int index);

/**
 * @brief 抓拍设备图片
 * @param index ONVIF设备列表的索引，索引从 0 开始
 * @param stream_type 码流类型（部分设备仅主码流支持抓图）
 * @param img_cb 图片数据回调函数
 * @param user_data 用户自定义数据
 * @return 错误码
 */
 onvif_error_e mg_onvif_capture_image(int index, onvif_stream_type_e stream_type, capture_image_callback img_cb, void* user_data);

 #endif // MG_ONVIF_CLIENT_H