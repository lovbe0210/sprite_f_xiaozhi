#ifndef _CAMERA_SERVICE_H_
#define _CAMERA_SERVICE_H_

#include "camera.h"
#include "device_state.h"
#include "image_id_cache.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <string>
#include <memory>
#include <mutex>
#include <deque>

// 前向声明（在 esp32_camera.h 中定义）
struct JpegFrameBuffer;

/**
 * @brief CameraService - 摄像头任务执行层
 *
 * 负责摄像头拍照和上传的核心服务，支持双模式触发：
 * - 主动模式：Idle状态，定时拍照用于智能唤醒
 * - 被动模式：Listening/Speaking状态拍照辅助语音交互
 *
 * 核心特性：
 * - 任务化封装：拍照→上传→返回结果 整体封装
 * - 简化接口：服务器自动保存图片，无需客户端管理ID
 * - 异步执行：不阻塞音频和其他任务
 * - 优雅降级：摄像头失败不影响核心功能
 */
class CameraService {
public:
    /**
     * @brief 任务参数
     *
     * 用于设置任务行为
     */
    struct TaskParams {
        std::string context;           ///< "active", "listening", "speaking"
        int delay_ms = 0;              ///< 执行延迟（被动模式需要）
        bool auto_cleanup = true;      ///< 自动清理过期ID

        TaskParams() = default;
        TaskParams(const std::string& ctx, int delay = 0)
            : context(ctx), delay_ms(delay) {}
    };

    /**
     * @brief 任务执行结果
     */
    struct TaskResult {
        bool success;                  ///< 任务是否成功
        std::string error_message;     ///< 错误信息
        uint64_t timestamp_ms;         ///< 时间戳
        std::string context;           ///< 上下文

        TaskResult() : success(false), timestamp_ms(0) {}
    };

    /**
     * @brief 配置结构（只包含用户需要调整的参数）
     */
    struct Config {
        int active_interval_ms = 100;      ///< 主动模式间隔（毫秒）
        int passive_delay_ms = 500;        ///< 被动模式延迟（毫秒）

        // 多帧拍摄配置
        int multi_frame_count = 3;          ///< 连续拍摄帧数

        Config() = default;
    };

    /**
     * @brief 统计信息
     */
    struct Statistics {
        uint32_t total_captures = 0;         ///< 总拍照次数
        uint32_t successful_captures = 0;    ///< 成功次数
        uint32_t failed_captures = 0;        ///< 失败次数
        uint32_t active_mode_captures = 0;   ///< 主动模式次数
        uint32_t passive_mode_captures = 0;  ///< 被动模式次数
        uint32_t upload_errors = 0;          ///< 上传错误次数

        void Reset() {
            total_captures = 0;
            successful_captures = 0;
            failed_captures = 0;
            active_mode_captures = 0;
            passive_mode_captures = 0;
            upload_errors = 0;
        }
    };

public:
    CameraService();
    ~CameraService();

    /**
     * @brief 初始化CameraService
     * @param camera Camera实例指针
     *
     * 必须在Start()之前调用
     */
    void Initialize(Camera* camera);

    /**
     * @brief 启动服务
     *
     * 启动后可以响应状态变化触发拍照
     */
    void Start();

    /**
     * @brief 停止服务
     *
     * 停止所有任务，等待当前任务完成
     */
    void Stop();

    /**
     * @brief 设备状态变化回调
     * @param prev 前一个状态
     * @param curr 当前状态
     *
     * 由DeviceStateEventManager调用
     */
    void OnDeviceStateChanged(DeviceState prev, DeviceState curr);

    /**
     * @brief 手动触发任务
     * @param params 任务参数
     * @return 任务执行结果
     *
     * 用于测试或手动拍照
     */
    inline TaskResult ExecuteTask(const TaskParams& params);

    /**
     * @brief 设置配置
     * @param config 配置对象
     */
    void SetConfig(const Config& config);

    /**
     * @brief 获取配置
     * @return 当前配置
     */
    Config GetConfig() const;

    /**
     * @brief 获取统计信息
     * @return 统计信息
     */
    Statistics GetStatistics() const;

private:
    /**
     * @brief 核心任务执行（原子操作）
     * @param params 任务参数
     * @return 任务执行结果
     *
     * 执行流程：
     * 1. 延迟（如需要）
     * 2. 拍摄原始帧
     * 3. JPEG编码
     * 4. HTTP上传
     */
    TaskResult CaptureAndUploadTask(const TaskParams& params);

    /**
     * @brief HTTP上传多帧到服务器
     * @param frames JPEG帧数组
     * @param frame_count 帧数量
     * @param context 上下文类型
     * @param device_state 设备状态
     * @return 是否成功
     */
    bool UploadMultipleFramesToServer(const JpegFrameBuffer* frames, size_t frame_count,
                                      const std::string& context,
                                      const char* device_state);

    // 主动模式任务
    void ActiveModeTask();
    void StartActiveMode();
    void StopActiveMode();

    // 被动模式任务
    static void PassiveModeTaskWrapper(void* arg);

private:
    Camera* camera_;                           ///< Camera实例
    Config config_;                            ///< 配置
    Statistics stats_;                         ///< 统计信息

    // 任务句柄
    TaskHandle_t active_mode_task_handle_ = nullptr;

    // 状态
    DeviceState current_state_ = kDeviceStateUnknown;
    bool service_running_ = false;
    bool active_mode_running_ = false;

    // 同步
    mutable std::mutex mutex_;
};

#endif  // _CAMERA_SERVICE_H_
