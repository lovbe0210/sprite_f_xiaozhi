# ESP32语音交互设备摄像头集成技术设计方案 v4.0

**版本**：v4.0
**日期**：2026-03-21
**状态**：实现阶段
**变更记录**：
- v4.0: **简化架构** - 移除 image_id 缓存机制，服务器自动保存图片
- v3.0: **架构重构** - 任务化封装 + ID缓存机制，完全解耦拍照和业务逻辑
- v2.1: 恢复主动模式（智能唤醒基础），实现真正的HTTP异步上传
- v2.0: 简化架构，移除过度设计的配置项，明确HTTP上传方案
- v1.0: 初始版本

---

## 目录

1. [项目概述](#项目概述)
2. [图片vs视频技术分析](#图片vs视频技术分析)
3. [架构设计：任务化封装](#架构设计任务化封装)
4. [CameraService核心设计](#cameraservice核心设计)
5. [HTTP接口设计](#http接口设计)
6. [业务层集成](#业务层集成)
7. [实施计划](#实施计划)
8. [性能和资源管理](#性能和资源管理)

---

## 项目概述

### 1.1 核心设计理念

**问题**：传统方案中拍照、上传、业务逻辑紧密耦合

**解决方案**：
1. **任务化封装**：拍照→上传→返回结果 整体封装
2. **服务器管理**：服务端自动保存和管理图片，无需客户端缓存ID
3. **完全解耦**：业务层只需知道上传成功与否

### 1.2 设计原则

1. **单一职责**：CameraService只负责拍照和上传
2. **简化接口**：不需要管理image_id，服务器处理一切
3. **异步优先**：所有耗时操作异步执行
4. **优雅降级**：摄像头失败不影响核心功能

---

## 图片vs视频技术分析

### 2.1 ESP32-S3 性能基准

基于 [Espressif官方性能数据](https://github.com/espressif/esp32-camera/issues/510)：

| 项目 | 性能指标 | 备注 |
|------|---------|------|
| JPEG编码时间（640x480） | ~50ms | 使用硬件加速 |
| 支持帧率 | 18-20 FPS | VGA分辨率 |
| JPEG压缩率 | 85-90% | 600KB → 50KB |
| CPU占用（单次） | <5% | 50ms/5秒 = 1% |

### 2.2 方案对比

#### 方案A：单张JPEG图片 ✅ 推荐

```
优势：
├── CPU占用低：50ms/次，可忽略
├── 功耗低：只在拍照时工作
├── 网络友好：30-50KB/次
├── 实现简单：现有代码已支持
└── 适合场景：人脸/手势检测（100ms间隔足够）

劣势：
└── 无法捕捉连续动作（但智能唤醒不需要）
```

#### 方案B：MJPEG视频流 ❌ 不推荐

```
优势：
└── 可捕捉连续动作

劣势：
├── CPU占用高：持续编码，影响音频
├── 功耗高：持续工作
├── 网络占用大：300-500KB/秒
├── 实现复杂：需要帧缓冲、流控
└── 不适合场景：智能唤醒（100ms间隔足够）
```

**技术决策**：**使用单张JPEG图片**

---

## 架构设计：任务化封装

### 3.1 整体架构

```
┌───────────────────────────────────────────────────────────────┐
│                         Application                          │
│  - 设备状态管理                                               │
│  - 业务逻辑（语音交互）                                       │
└───────────────────────────┬───────────────────────────────────┘
                            │
                            │ 状态变化触发
                            ▼
┌───────────────────────────────────────────────────────────────┐
│                   CameraService (任务执行层)                 │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ CaptureAndUploadTask (原子任务)                      │    │
│  │  1. Camera::CaptureRawFrame()                        │    │
│  │  2. JPEG编码（独立线程，~50ms）                      │    │
│  │  3. HTTP POST上传到服务器                            │    │
│  │  4. 接收响应（检测/保存结果）                         │    │
│  └──────────────────────────────────────────────────────┘    │
│                                                             │
│  双模式触发：                                                │
│  ├─ 主动模式：Idle状态，定时触发                            │
│  └─ 被动模式：Listening/Speaking状态触发                    │
└───────────────────────────┬───────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────┐
│                      HTTP POST /api/camera/upload            │
│                                                              │
│  请求：multipart/form-data                                    │
│  ├── context: "active" / "listening" / "speaking"            │
│  ├── timestamp: 1736789123456                                │
│  └── files: 多帧JPEG数据                                      │
│                                                              │
│  响应（根据上下文不同）：                                      │
│  - active: { detection_result: { activate: bool } }         │
│  - speaking: { detection_result: { interrupt: bool } }      │
│  - listening: {} (服务器自动保存)                            │
└───────────────────────────────────────────────────────────────┘
```

### 3.2 数据流设计

#### 主动模式（智能唤醒检测）

```
t0: 状态 → Idle
    ↓
t1: [定时器触发，默认100ms]
    ↓
t2: CameraService 启动 CaptureAndUploadTask
    ├── 拍摄多帧（100ms）
    ├── JPEG编码（50ms/帧）
    ├── HTTP上传（2-3秒，异步）
    └── 返回检测结果
    ↓
t3: 服务端处理
    ├── 人脸检测 → 匹配用户
    ├── 手势识别 → 理解意图
    └── 决策：是否需要唤醒
    ↓
t4: 如需唤醒，WebSocket发送指令
    {
      "type": "wake_up",
      "user_id": "user_123",
      "message": "你好，张三"
    }
```

#### 被动模式（语音打断检测）

```
设备Speaking状态 → 用户打断
    ↓
CameraService 启动 CaptureAndUploadTask
    ├── 拍摄 → 上传
    └── 返回检测结果 { interrupt: true/false }
    ↓
如果 interrupt=true，服务端通过WebSocket通知
    {
      "type": "interrupt",
      "reason": "用户挥手"
    }
    ↓
设备切换到 Listening 状态
```

#### 聆听模式（服务器自动保存）

```
用户按下唤醒 → Listening状态
    ↓
CameraService 启动 CaptureAndUploadTask
    ├── 拍摄 → 上传
    └── 返回成功（无需解析具体内容）
    ↓
服务器自动保存图片用于后续AI处理
    ├── 结合音频和图片进行理解
    └── 图片可选，失败不影响交互
```

### 3.3 简化优势对比

| 场景 | v3.0 方案（有ID缓存） | v4.0 方案（简化） |
|------|---------------------|-----------------|
| 业务层需要知道 | image_id 管理 | **仅上传状态** |
| 内存占用 | 需要缓存队列 | **零额外内存** |
| 拍照失败影响 | 需要清理过期ID | **无状态，无影响** |
| 服务端获取图片 | 根据ID拉取 | **服务器自动管理** |
| 代码复杂度 | 需要缓存管理逻辑 | **极简** |

---

## CameraService核心设计

### 4.1 CameraService（任务执行层）

**文件**：`main/camera_service.h`

```cpp
#pragma once

#include "camera.h"
#include "device_state.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <string>
#include <memory>
#include <mutex>

// 前向声明（在 esp32_camera.h 中定义）
struct JpegFrameBuffer;

/**
 * @brief CameraService - 摄像头任务执行层
 */
class CameraService {
public:
    struct TaskParams {
        std::string context;           ///< "active", "listening", "speaking"
        int delay_ms = 0;              ///< 执行延迟（被动模式需要）
        bool auto_cleanup = true;      ///< 保留参数（未使用）

        TaskParams() = default;
        TaskParams(const std::string& ctx, int delay = 0)
            : context(ctx), delay_ms(delay) {}
    };

    struct TaskResult {
        bool success;                  ///< 任务是否成功
        std::string error_message;     ///< 错误信息
        uint64_t timestamp_ms;         ///< 时间戳
        std::string context;           ///< 上下文

        TaskResult() : success(false), timestamp_ms(0) {}
    };

    struct Config {
        int active_interval_ms = 100;      ///< 主动模式间隔（毫秒）
        int passive_delay_ms = 500;        ///< 被动模式延迟（毫秒）
        int multi_frame_count = 3;         ///< 连续拍摄帧数

        Config() = default;
    };

    struct Statistics {
        uint32_t total_captures = 0;
        uint32_t successful_captures = 0;
        uint32_t failed_captures = 0;
        uint32_t active_mode_captures = 0;
        uint32_t passive_mode_captures = 0;
        uint32_t upload_errors = 0;

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

    void Initialize(Camera* camera);
    void Start();
    void Stop();

    void OnDeviceStateChanged(DeviceState prev, DeviceState curr);
    inline TaskResult ExecuteTask(const TaskParams& params);

    void SetConfig(const Config& config);
    Config GetConfig() const;
    Statistics GetStatistics() const;

private:
    TaskResult CaptureAndUploadTask(const TaskParams& params);

    bool UploadMultipleFramesToServer(const JpegFrameBuffer* frames, size_t frame_count,
                                      const std::string& context,
                                      const char* device_state);

    void ActiveModeTask();
    void StartActiveMode();
    void StopActiveMode();
    static void PassiveModeTaskWrapper(void* arg);

private:
    Camera* camera_;
    Config config_;
    Statistics stats_;

    TaskHandle_t active_mode_task_handle_ = nullptr;
    DeviceState current_state_ = kDeviceStateUnknown;
    bool service_running_ = false;
    bool active_mode_running_ = false;
    mutable std::mutex mutex_;
};
```

---

## HTTP接口设计

### 5.1 服务端接口

#### POST /api/camera/upload

**请求**：
```http
POST /api/camera/upload HTTP/1.1
Content-Type: multipart/form-data; boundary=----ESP32_CAMERA_BOUNDARY
Device-Id: XX:XX:XX:XX:XX:XX
Client-Id: uuid-from-nvs
Authorization: Bearer <token>

------ESP32_CAMERA_BOUNDARY
Content-Disposition: form-data; name="context"

active
------ESP32_CAMERA_BOUNDARY
Content-Disposition: form-data; name="device_state"

idle
------ESP32_CAMERA_BOUNDARY
Content-Disposition: form-data; name="files"; filename="frame_0.jpg"
Content-Type: image/jpeg

<JPEG binary data>
------ESP32_CAMERA_BOUNDARY--
```

**响应**：

服务器根据不同的上下文返回不同的响应格式：

#### 模式1：speaking 语音打断检测模式

```json
{
  "code": 200,
  "message": "操作成功",
  "data": {
    "interrupt": "true"
  }
}
```

#### 模式2：idle 唤醒检测模式

```json
{
  "code": 200,
  "message": "操作成功",
  "data": {
    "activate": true
  }
}
```

#### 模式3：listening 设备聆听模式

```json
{
  "code": 200,
  "message": "操作成功",
  "data": {}
}
```

**字段说明**：
- `code`: 200表示成功
- `message`: 状态描述
- `data`: 检测结果（speaking/idle模式）
  - `interrupt`: 是否打断当前说话状态（speaking模式）
  - `activate`: 是否激活设备（idle模式）
  - `reason`: 检测原因描述

---

## 业务层集成

### 6.1 在Application中使用

**修改文件**：`main/application.cc`

```cpp
class Application {
private:
    std::unique_ptr<CameraService> camera_service_;
};

void Application::Initialize() {
    camera_service_ = std::make_unique<CameraService>();
    auto* camera = Board::GetInstance().GetCamera();
    if (camera != nullptr) {
        camera_service_->Initialize(camera);

        DeviceStateEventManager::RegisterStateChangeCallback(
            [this](DeviceState prev, DeviceState curr) {
                if (camera_service_) {
                    camera_service_->OnDeviceStateChanged(prev, curr);
                }
            }
        );

        camera_service_->Start();
    }
}
```

### 6.2 优雅降级

```cpp
// 摄像头失败不影响核心功能
// CameraService内部已经处理了所有错误情况
// 业务层无需关心摄像头状态
```

---

## 实施计划

### 阶段1：简化架构 ✅ 已完成

- [x] 移除 ImageIdCache 类
- [x] 移除 image_id 相关代码
- [x] 简化 CameraService 接口
- [x] 更新响应解析逻辑

**完成日期**：2026-03-21

### 阶段2：修复内存问题 ✅ 已完成

- [x] 添加内存分配重试机制
- [x] 添加堆内存清理
- [x] 添加详细错误日志

**完成日期**：2026-03-21

### 阶段3：服务端对接 🔄 进行中

- [ ] 服务端实现三种响应模式
- [ ] 端到端测试
- [ ] 性能优化

---

## 性能和资源管理

### 8.1 性能目标

| 指标 | 目标值 |
|------|--------|
| 拍照+上传耗时 | <3秒 |
| 内存占用 | <100KB |
| CPU占用 | <5% |

### 8.2 优势总结

| 维度 | v3.0 方案 | v4.0 方案 |
|------|-----------|-----------|
| 业务层复杂度 | 需要管理 ID | **无需管理** |
| 内存占用 | 需要缓存队列 | **零额外内存** |
| 服务端改造 | 需要返回 ID | **服务器管理** |
| 代码量 | 较多 | **更少** |
| 维护成本 | 中等 | **低** |

---

## 关键文件清单

### 核心文件
- `main/camera_service.h` - CameraService类定义（简化版）
- `main/camera_service.cc` - CameraService实现
- `main/boards/common/esp32_camera.cc` - 摄像头驱动（含内存优化）

### 文档
- `docs/camera_integration_design.md` - 本技术设计文档（v4.0）
- `docs/camera_service_README.md` - 集成指南

---

## 附录

### A. WebSocket消息示例

**服务端→客户端（主动唤醒）**：
```json
{
  "type": "wake_up",
  "user_id": "user_123",
  "message": "你好，张三"
}
```

**服务端→客户端（打断提示）**：
```json
{
  "type": "interrupt",
  "reason": "检测到用户挥手"
}
```

---

**文档结束**
