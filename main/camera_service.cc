#include "camera_service.h"
#include "esp_log.h"
#include "system_info.h"
#include "board.h"
#include "settings.h"
#include "jpg/image_to_jpeg.h"
#include "boards/common/esp32_camera.h"
#include <cJSON.h>
#include <esp_timer.h>
#include <vector>

static const char* TAG = "CameraService";

// 静态缓冲区：复用JPEG数据向量，避免每次拍照都分配/释放内存
static std::vector<uint8_t> g_jpeg_data_buffer;

CameraService::CameraService() {
    ESP_LOGI(TAG, "CameraService created");
}

CameraService::~CameraService() {
    if (service_running_) {
        Stop();
    }
    ESP_LOGI(TAG, "CameraService destroyed");
}

void CameraService::Initialize(Camera* camera) {
    if (camera == nullptr) {
        ESP_LOGE(TAG, "Camera is null, initialization failed");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 初始化 Camera 指针
    camera_ = camera;

    // 创建 ImageIdCache 实例（内部管理）
    cache_ = std::make_unique<ImageIdCache>(20);  // 默认缓存20个ID

    ESP_LOGI(TAG, "CameraService initialized with internal ImageIdCache");
}

std::string CameraService::GetLatestImageId(const std::string& context) {
    if (cache_ == nullptr) {
        ESP_LOGW(TAG, "ImageIdCache not initialized");
        return "";
    }
    return cache_->GetLatestId(context);
}

ImageIdRecord CameraService::GetLatestImageRecord(const std::string& context) {
    if (cache_ == nullptr) {
        ESP_LOGW(TAG, "ImageIdCache not initialized");
        return ImageIdRecord{"", 0, "", ""};
    }
    return cache_->GetLatestRecord(context);
}

void CameraService::Start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (service_running_) {
        ESP_LOGW(TAG, "CameraService already running");
        return;
    }

    if (camera_ == nullptr || cache_ == nullptr) {
        ESP_LOGE(TAG, "CameraService not initialized, call Initialize() first");
        return;
    }

    service_running_ = true;
    ESP_LOGI(TAG, "CameraService started");
}

void CameraService::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!service_running_) {
        return;
    }

    // 停止主动模式
    StopActiveMode();

    service_running_ = false;
    ESP_LOGI(TAG, "CameraService stopped");
}

void CameraService::OnDeviceStateChanged(DeviceState prev, DeviceState curr) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_state_ = curr;

    ESP_LOGI(TAG, "State changed: %d -> %d", prev, curr);

    if (!service_running_) {
        return;
    }

    // 被动模式触发
    switch (curr) {
        case kDeviceStateListening:
        case kDeviceStateSpeaking: {
            // 创建一次性任务参数
            struct TaskArg {
                CameraService* service;
                std::string context;
            };

            auto* arg = new TaskArg{this, curr == kDeviceStateListening ? "listening" : "speaking"};

            BaseType_t ret = xTaskCreate(
                PassiveModeTaskWrapper,
                "camera_passive",
                8192,
                arg,
                2,  // 优先级2，低于音频任务
                nullptr
            );

            if (ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create passive mode task");
                delete arg;
            }
            break;
        }

        default:
            break;
    }

    // 主动模式触发
    if (curr == kDeviceStateIdle) {
        StartActiveMode();
    } else {
        StopActiveMode();
    }
}

CameraService::TaskResult CameraService::ExecuteTask(const TaskParams& params) {
    return CaptureAndUploadTask(params);
}

void CameraService::SetConfig(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    ESP_LOGI(TAG, "Config updated: active_interval=%d, passive_delay=%d",
            config.active_interval_ms, config.passive_delay_ms);
}

CameraService::Config CameraService::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

CameraService::Statistics CameraService::GetStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

CameraService::TaskResult CameraService::CaptureAndUploadTask(const TaskParams& params) {
    TaskResult result;
    result.timestamp_ms = esp_timer_get_time() / 1000;
    result.context = params.context;

    ESP_LOGI(TAG, "=== Task start: context=%s, delay=%d ===", params.context.c_str(), params.delay_ms);
    ESP_LOGI(TAG, "Executing task: context=%s, delay=%d",
            params.context.c_str(), params.delay_ms);

    // 1. 延迟（如需要）
    if (params.delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(params.delay_ms));
    }

    // 2. 拍摄多帧填充循环缓冲区
    if (camera_ == nullptr) {
        ESP_LOGE(TAG, "camera_ is null!");
        result.success = false;
        result.error_message = "Camera not initialized";
        return result;
    }

    // 获取 Esp32Camera 实例以访问循环缓冲区
    auto* esp32_camera = static_cast<Esp32Camera*>(camera_);

    // 连续拍摄5帧（JPEG编码本身约150ms，无需额外长延迟）
    for (int i = 0; i < 5; i++) {
        if (!camera_->CaptureRawFrame()) {
            ESP_LOGW(TAG, "Frame %d capture failed, continuing...", i);
        }
        // 只给短暂延迟让系统调度，JPEG编码本身已占大部分时间
        if (i < 4) {  // 最后一次不需要等待
            vTaskDelay(pdMS_TO_TICKS(10));  // 10ms间隔（原200ms）
        }
    }

    ESP_LOGI(TAG, "Captured 5 frames for circular buffer");

    // 3. 从循环缓冲区获取最近的5帧 JPEG 数据
    JpegFrameBuffer frames[5];
    size_t frame_count = esp32_camera->GetJpegCircularBuffer().GetRecentFrames(5, frames);

    if (frame_count == 0) {
        result.success = false;
        result.error_message = "No frames in circular buffer";
        stats_.failed_captures++;
        return result;
    }

    ESP_LOGI(TAG, "Got %u frames from circular buffer", (unsigned int)frame_count);

    // 4. HTTP上传多帧，获取image_id
    std::string image_id;
    if (!UploadMultipleFramesToServer(frames, frame_count, params.context, image_id)) {
        result.success = false;
        result.error_message = "Failed to upload to server";
        stats_.failed_captures++;
        stats_.upload_errors++;
        return result;
    }

    // 5. 缓存image_id
    if (!image_id.empty()) {
        ImageIdRecord record;
        record.id = image_id;
        record.timestamp_ms = result.timestamp_ms;
        record.context = params.context;

        cache_->Add(record);

        stats_.successful_captures++;

        if (params.context == "active") {
            stats_.active_mode_captures++;
        } else {
            stats_.passive_mode_captures++;
        }

        ESP_LOGI(TAG, "Task completed: image_id=%s (%u frames)", image_id.c_str(), (unsigned int)frame_count);
    }

    stats_.total_captures++;
    result.success = true;
    result.image_id = image_id;

    // 任务完成，等待一段时间让系统整理内存
    // 这样可以减少与语音识别等任务的内存竞争
    vTaskDelay(pdMS_TO_TICKS(200));

    // 7. 自动清理过期ID（如需要）
    if (params.auto_cleanup) {
        CleanupExpiredIds();
    }

    // 清空JPEG数据缓冲区（保留容量供下次使用）
    g_jpeg_data_buffer.clear();

    return result;
}

bool CameraService::UploadToServer(const uint8_t* data, size_t len,
                                  const std::string& context,
                                  std::string& out_image_id) {
    // 从Settings获取上传URL和token（与WebSocket使用相同的api_server配置）
    Settings settings("api_server", false);
    std::string upload_url = settings.GetString("url");

    // 移除末尾的斜杠（如果有）
    if (!upload_url.empty() && upload_url.back() == '/') {
        upload_url.pop_back();
    }

    std::string upload_token = settings.GetString("token");

    // 首先检查 URL 配置
    if (upload_url.empty()) {
        ESP_LOGE(TAG, "Upload URL not configured in settings (api_server.url), please configure it first");
        return false;
    }

    // 然后检查数据
    if (data == nullptr) {
        ESP_LOGE(TAG, "Camera data is nullptr, cannot upload");
        return false;
    }

    if (len == 0) {
        ESP_LOGE(TAG, "Camera data length is 0, cannot upload. Check if Camera::GetFrameData() is implemented");
        return false;
    }

    // 实际HTTP上传
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network is null");
        return false;
    }

    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    // 配置HTTP（使用静态缓冲区避免字符串拼接分配）
    static char content_type_buf[128];
    snprintf(content_type_buf, sizeof(content_type_buf), "multipart/form-data; boundary=%s", boundary.c_str());
    http->SetHeader("Content-Type", content_type_buf);

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    if (!upload_token.empty()) {
        static char auth_buf[256];
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", upload_token.c_str());
        http->SetHeader("Authorization", auth_buf);
    }

    ESP_LOGI(TAG, "Uploading to: %s", upload_url.c_str());

    if (!http->Open("POST", upload_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    // 构造multipart body
    // context字段
    {
        std::string context_field;
        context_field += "--" + boundary + "\r\n";
        context_field += "Content-Disposition: form-data; name=\"context\"\r\n";
        context_field += "\r\n";
        context_field += context + "\r\n";
        http->Write(context_field.c_str(), context_field.size());
    }

    // timestamp字段
    {
        std::string timestamp_field;
        timestamp_field += "--" + boundary + "\r\n";
        timestamp_field += "Content-Disposition: form-data; name=\"timestamp\"\r\n";
        timestamp_field += "\r\n";
        timestamp_field += std::to_string(esp_timer_get_time() / 1000) + "\r\n";
        http->Write(timestamp_field.c_str(), timestamp_field.size());
    }

    // 文件头部
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    // 发送JPEG数据
    http->Write((const char*)data, len);

    // 结束边界
    {
        std::string end_boundary;
        end_boundary += "\r\n--" + boundary + "--\r\n";
        http->Write(end_boundary.c_str(), end_boundary.size());
    }
    http->Write("", 0);  // 结束请求

    // 解析响应，提取image_id
    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP upload failed with status: %d", status_code);
        http->Close();
        return false;
    }

    // 读取响应体
    std::string response = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "Server response status: %d, length: %u bytes", status_code, (unsigned int)response.length());

    // 检查响应是否为空
    if (response.empty()) {
        ESP_LOGE(TAG, "Server returned empty response (status: %d)", status_code);
        return false;
    }

    // 打印响应内容（只打印可打印字符，避免崩溃）
    constexpr size_t max_print_len = 500;
    size_t print_len = std::min(response.length(), max_print_len);
    std::string printable;
    for (size_t i = 0; i < print_len; i++) {
        unsigned char c = response[i];
        if (c >= 32 && c <= 126) {
            printable += c;
        } else if (c == '\n') {
            printable += "\\n";
        } else if (c == '\r') {
            printable += "\\r";
        } else if (c == '\t') {
            printable += "\\t";
        } else {
            printable += "?";  // 替换不可打印字符
        }
    }
    ESP_LOGI(TAG, "Response printable: %s", printable.c_str());

    // JSON解析
    // 服务器返回格式: {"code":200,"message":"xxx","data":{"image_id":"xxx","detection_result":{...}}}
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    // 检查 code 字段（200 表示成功）
    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (code == nullptr || !cJSON_IsNumber(code) || code->valueint != 200) {
        int code_val = code ? code->valueint : -1;
        ESP_LOGE(TAG, "Response indicates failure: code=%d (expected 200)", code_val);
        cJSON_Delete(root);
        return false;
    }

    // 获取 data 对象
    cJSON* data_obj = cJSON_GetObjectItem(root, "data");
    if (data_obj == nullptr) {
        ESP_LOGE(TAG, "No data object in response");
        cJSON_Delete(root);
        return false;
    }

    // 从 data 中获取 image_id
    cJSON* image_id = cJSON_GetObjectItem(data_obj, "image_id");
    if (image_id == nullptr || !cJSON_IsString(image_id)) {
        ESP_LOGE(TAG, "No image_id in data object");
        cJSON_Delete(root);
        return false;
    }

    out_image_id = image_id->valuestring;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Upload successful: image_id=%s", out_image_id.c_str());
    return true;
}

bool CameraService::UploadMultipleFramesToServer(const JpegFrameBuffer* frames, size_t frame_count,
                                                   const std::string& context,
                                                   std::string& out_image_id) {
    // 从Settings获取上传URL和token（与WebSocket使用相同的api_server配置）
    Settings settings("api_server", false);
    std::string upload_url = settings.GetString("url");

    // 移除末尾的斜杠（如果有）
    if (!upload_url.empty() && upload_url.back() == '/') {
        upload_url.pop_back();
    }

    std::string upload_token = settings.GetString("token");

    // 首先检查 URL 配置
    if (upload_url.empty()) {
        ESP_LOGE(TAG, "Upload URL not configured in settings (api_server.url), please configure it first");
        return false;
    }

    // 检查帧数据
    if (frames == nullptr || frame_count == 0) {
        ESP_LOGE(TAG, "No frames to upload");
        return false;
    }

    // 实际HTTP上传
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network is null");
        return false;
    }

    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    // 配置HTTP（使用静态缓冲区避免字符串拼接分配）
    static char content_type_buf[128];
    snprintf(content_type_buf, sizeof(content_type_buf), "multipart/form-data; boundary=%s", boundary.c_str());
    http->SetHeader("Content-Type", content_type_buf);

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    if (!upload_token.empty()) {
        static char auth_buf[256];
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", upload_token.c_str());
        http->SetHeader("Authorization", auth_buf);
    }

    ESP_LOGI(TAG, "Uploading %u frames to: %s", (unsigned int)frame_count, upload_url.c_str());

    if (!http->Open("POST", upload_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    // 构造multipart body
    // context字段
    {
        std::string context_field;
        context_field += "--" + boundary + "\r\n";
        context_field += "Content-Disposition: form-data; name=\"context\"\r\n";
        context_field += "\r\n";
        context_field += context + "\r\n";
        http->Write(context_field.c_str(), context_field.size());
    }

    // timestamp字段
    {
        std::string timestamp_field;
        timestamp_field += "--" + boundary + "\r\n";
        timestamp_field += "Content-Disposition: form-data; name=\"timestamp\"\r\n";
        timestamp_field += "\r\n";
        timestamp_field += std::to_string(esp_timer_get_time() / 1000) + "\r\n";
        http->Write(timestamp_field.c_str(), timestamp_field.size());
    }

    // 上传多帧文件
    for (size_t i = 0; i < frame_count; i++) {
        const JpegFrameBuffer& frame = frames[i];
        if (!frame.valid || frame.data == nullptr || frame.len == 0) {
            ESP_LOGW(TAG, "Skipping invalid frame %u", (unsigned int)i);
            continue;
        }

        // 文件头部（每帧都是 name="files"，服务端用数组接收）
        {
            char filename_buf[64];
            snprintf(filename_buf, sizeof(filename_buf), "frame_%u.jpg", (unsigned int)i);

            std::string file_header;
            file_header += "--" + boundary + "\r\n";
            file_header += "Content-Disposition: form-data; name=\"files\"; filename=\"";
            file_header += filename_buf;
            file_header += "\"\r\n";
            file_header += "Content-Type: image/jpeg\r\n";
            file_header += "\r\n";
            http->Write(file_header.c_str(), file_header.size());
        }

        // 发送JPEG数据
        http->Write((const char*)frame.data, frame.len);

        // 每个part结束后需要发送 \r\n
        http->Write("\r\n", 2);

        ESP_LOGI(TAG, "Uploaded frame %u: %u bytes", (unsigned int)i, (unsigned int)frame.len);
    }

    // 结束边界
    {
        std::string end_boundary;
        end_boundary += "\r\n--" + boundary + "--\r\n";
        http->Write(end_boundary.c_str(), end_boundary.size());
    }
    http->Write("", 0);  // 结束请求

    // 解析响应，提取image_id
    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP upload failed with status: %d", status_code);
        http->Close();
        return false;
    }

    // 读取响应体
    std::string response = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "Server response status: %d, length: %u bytes", status_code, (unsigned int)response.length());

    // 检查响应是否为空
    if (response.empty()) {
        ESP_LOGE(TAG, "Server returned empty response (status: %d)", status_code);
        return false;
    }

    // JSON解析
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    // 检查 code 字段（200 表示成功）
    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (code == nullptr || !cJSON_IsNumber(code) || code->valueint != 200) {
        int code_val = code ? code->valueint : -1;
        ESP_LOGE(TAG, "Response indicates failure: code=%d (expected 200)", code_val);
        cJSON_Delete(root);
        return false;
    }

    // 获取 data 对象
    cJSON* data_obj = cJSON_GetObjectItem(root, "data");
    if (data_obj == nullptr) {
        ESP_LOGE(TAG, "No data object in response");
        cJSON_Delete(root);
        return false;
    }

    // 从 data 中获取 image_id
    cJSON* image_id = cJSON_GetObjectItem(data_obj, "image_id");
    if (image_id == nullptr || !cJSON_IsString(image_id)) {
        ESP_LOGE(TAG, "No image_id in data object");
        cJSON_Delete(root);
        return false;
    }

    out_image_id = image_id->valuestring;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Upload successful: image_id=%s (%u frames)", out_image_id.c_str(), (unsigned int)frame_count);
    return true;
}

void CameraService::ActiveModeTask() {
    ESP_LOGI(TAG, "Active mode task started, interval: %d ms", config_.active_interval_ms);

    // 等待相机初始化完成（相机在后台初始化，需要约600ms稳定时间）
    // 使用初始延迟确保第一次拍照时相机已就绪
    vTaskDelay(pdMS_TO_TICKS(1000));  // 初始延迟1秒

    while (active_mode_running_ && service_running_) {
        vTaskDelay(pdMS_TO_TICKS(config_.active_interval_ms));

        if (!active_mode_running_ || !service_running_) {
            break;
        }

        // 确认仍在idle状态
        if (current_state_ != kDeviceStateIdle) {
            continue;
        }

        // 执行任务
        TaskParams params;
        params.context = "active";
        params.delay_ms = 0;
        auto result = ExecuteTask(params);

        if (result.success) {
            ESP_LOGI(TAG, "Active mode: image_id=%s", result.image_id.c_str());
        } else {
            ESP_LOGW(TAG, "Active mode failed: %s", result.error_message.c_str());
        }

        // 自动清理过期ID
        CleanupExpiredIds();
    }

    ESP_LOGI(TAG, "Active mode task stopped");
}

void CameraService::StartActiveMode() {
    if (active_mode_running_) {
        return;
    }

    active_mode_running_ = true;

    BaseType_t ret = xTaskCreate(
        [](void* arg) {
            auto* service = static_cast<CameraService*>(arg);
            service->ActiveModeTask();
            vTaskDelete(NULL);
        },
        "camera_active",
        8192,
        this,
        2,  // 优先级2
        &active_mode_task_handle_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create active mode task");
        active_mode_running_ = false;
        return;
    }

    ESP_LOGI(TAG, "Active mode started");
}

void CameraService::StopActiveMode() {
    if (!active_mode_running_) {
        return;
    }

    active_mode_running_ = false;
    ESP_LOGI(TAG, "Active mode stopped");
}

void CameraService::PassiveModeTaskWrapper(void* arg) {
    struct TaskArg {
        CameraService* service;
        std::string context;
    };

    auto* task_arg = static_cast<TaskArg*>(arg);
    CameraService* service = task_arg->service;
    std::string context = task_arg->context;
    delete task_arg;

    TaskParams params(context, service->config_.passive_delay_ms);
    auto result = service->ExecuteTask(params);

    if (result.success) {
        ESP_LOGI(TAG, "Passive mode (%s): image_id=%s",
                context.c_str(), result.image_id.c_str());
    } else {
        ESP_LOGW(TAG, "Passive mode (%s) failed: %s",
                context.c_str(), result.error_message.c_str());
    }

    vTaskDelete(NULL);
}

void CameraService::CleanupExpiredIds() {
    // 硬编码60秒过期时间
    if (cache_ != nullptr) {
        cache_->Cleanup(60000);
    }
}
