# ESP32摄像头服务集成指南

## 快速开始

### 1. 配置ESP32

在 `sdkconfig` 或 `menuconfig` 中配置摄像头服务：

```bash
idf.py menuconfig
```

导航到 `Xiaozhi Assistant -> Camera Configuration`：

```
Enable Camera Service:                    y
Active Mode Interval (ms):               5000
Passive Mode Delay (ms):                  500
HTTP Upload Timeout (ms):                10000
Image ID Cache Max Size:                 20
Image ID Cache Max Age (ms):             60000
```

**注意**：图片上传URL和Token**不在这里配置**，而是通过OTA响应中的`api_server`对象配置（与WebSocket使用相同的配置）。

### 2. 配置api_server（与WebSocket相同）

图片上传URL和Token通过OTA响应中的`api_server`对象配置，与WebSocket使用相同的配置方式：

```json
{
  "api_server": {
    "url": "https://your-api.com",
    "token": "your-bearer-token-here"
  },
  "websocket": [...]
}
```

**配置逻辑**：
1. ESP32发起OTA请求
2. 服务器响应包含`api_server`对象（包含url和token）
3. ESP32将url和token存储到Settings("api_server")
4. CameraService从Settings读取url和token
5. 上传URL = api_server.url + "/api/camera/upload"

这种方式的好处：
- 与WebSocket使用相同的配置来源
- 统一管理服务器地址和认证
- 便于切换环境（开发/生产）

### 3. 启动服务端

#### 方法A：使用Python示例（开发测试）

```bash
cd docs
pip install flask opencv-python
python server_example.py
```

服务将运行在 `http://localhost:5000`

#### 方法B：部署到生产环境

参考 `server_example.py` 的实现，使用您选择的框架：
- **Python**: Flask, FastAPI, Django
- **Node.js**: Express, Fastify
- **Go**: Gin, Echo
- **Java**: Spring Boot

### 3. 测试接口

```bash
# 使用提供的测试脚本
cd docs
chmod +x test_camera_upload.sh
./test_camera_upload.sh test_image.jpg
```

### 4. 测试接口

```bash
# 使用提供的测试脚本
cd docs
chmod +x test_camera_upload.sh
./test_camera_upload.sh test_image.jpg
```

### 5. 烧录ESP32

```bash
idf.py build flash monitor
```

## WebSocket消息格式

### 客户端发送

#### 1. 开始监听（附带image_id）

```json
{
  "session_id": "...",
  "type": "listen",
  "state": "start",
  "mode": "auto"
}
```

500ms后发送：
```json
{
  "session_id": "...",
  "type": "image_id",
  "context": "listening",
  "image_id": "img_1234567890_listening_abc123"
}
```

#### 2. 开始说话（附带image_id）

```json
{
  "type": "tts",
  "state": "start"
}
```

200ms后发送：
```json
{
  "session_id": "...",
  "type": "image_id",
  "context": "speaking",
  "image_id": "img_1234567890_speaking_def456"
}
```

### 服务端发送

#### 主动唤醒

```json
{
  "type": "wake_up",
  "trigger_image_id": "img_1234567890_active_ghi789",
  "user_id": "user_123",
  "message": "你好，张三",
  "action": "start_listening"
}
```

## HTTP接口规范

### POST /api/camera/upload

上传图片

**请求**：
```http
POST /api/camera/upload HTTP/1.1
Content-Type: multipart/form-data; boundary=----ESP32_CAMERA_BOUNDARY
Device-Id: XX:XX:XX:XX:XX:XX
Client-Id: uuid-from-nvs
Authorization: Bearer <token>

------ESP32_CAMERA_BOUNDARY
Content-Disposition: form-data; name="context"

listening
------ESP32_CAMERA_BOUNDARY
Content-Disposition: form-data; name="timestamp"

1736789123456
------ESP32_CAMERA_BOUNDARY
Content-Disposition: form-data; name="file"; filename="camera.jpg"
Content-Type: image/jpeg

<JPEG binary data, 30-50KB>
------ESP32_CAMERA_BOUNDARY--
```

**响应**：

服务器根据不同的场景返回不同的响应格式：

#### 模式1：speaking 语音打断检测模式


```json
{
  "code": 200,
  "message": "操作成功",
  "data": {
    "detection_result": {
      "interrupt": "true",
      "reason": null,
      "confidence": 0.95
    }
  }
}
```

#### 模式2：ilde 唤醒检测模式


```json
{
  "code": 200,
  "message": "操作成功",
  "data": {
    "detection_result": {
      "activate": false,
      "reason": null,
      "confidence": 0.0
    }
  }
}
```

#### 模式3：listening 设备聆听模式，由服务器自动保存listening状态期间的动作及交互


```json
{
  "code": 200,
  "message": "操作成功",
  "data": {
  }
}
```

**字段说明**：
- `code`: 200表示成功
- `message`: 状态描述
- `data.detection_result`: 检测结果
  - `interrupt`: 是否打断当前设备说话状态，切换为listening状态
  - `activate`：是否激活设备，设备状态切换为listening状态
  - `reason`: 简短描述唤醒的原因
  - `confidence`: 检测置信度（0.0-1.0）

### GET /api/camera/image/<image_id>

获取图片

**请求**：
```http
GET /api/camera/image/img_1736789123456_listening_abc123
```

**响应**：
- Content-Type: image/jpeg
- JPEG二进制数据

### GET /api/camera/list

列出所有图片

**响应**：
```json
{
  "success": true,
  "images": [
    {
      "image_id": "img_1736789123456_active_xyz",
      "size": 45678,
      "timestamp": "2025-02-06T12:34:56",
      "url": "http://api.example.com/camera/image/img_1736789123456_active_xyz"
    }
  ]
}
```

## image_id格式

```
img_<timestamp>_<context>_<unique_id>

例如：
- img_1736789123456_active_abc123  (主动模式)
- img_1736789123456_listening_def456  (被动模式，监听中)
- img_1736789123456_speaking_ghi789  (被动模式，说话中)
```

## 工作流程

### 主动模式（智能唤醒）

```
ESP32 (Idle状态)
  ↓ 每5秒拍照
  ↓ HTTP POST /api/camera/upload
  ↓
服务端
  ├─ 人脸检测
  ├─ 匹配用户
  └─ 如果检测到已知用户
     ↓
  WebSocket发送wake_up
     {
       "type": "wake_up",
       "trigger_image_id": "img_xxx",
       "user_id": "user_123",
       "message": "你好，张三"
     }
     ↓
ESP32
  └─ 收到唤醒指令
     └─ 进入Listening状态
```

### 被动模式（语音交互辅助）

```
用户按下唤醒 → ESP32 Listening状态
  ↓ 500ms延迟
  ↓ 拍照 → HTTP上传
  ↓
服务端
  ├─ 接收image_id
  ├─ 下载原图（可选）
  └─ AI处理（结合音频和图片）
     └─ 更精准的响应
```

## 故障排查

### 1. 图片上传失败

检查ESP32日志：
```
I (123456) CameraService: Upload failed with status: 500
```

可能原因：
- URL配置错误
- 网络连接问题
- 服务端未启动
- 文件过大

### 2. 无法接收image_id

检查WebSocket消息：
```json
{
  "type": "image_id",
  "context": "listening",
  "image_id": ""  // 空表示拍照失败
}
```

可能原因：
- 摄像头未初始化
- 拍照超时
- 上传超时

### 3. 主动模式不工作

检查：
1. 设备是否在Idle状态
2. 配置：`CONFIG_CAMERA_SERVICE_ENABLED=y`
3. 日志：`CameraService initialized and started`

## 配置说明

### api_server配置（与WebSocket相同）

图片上传URL和Token通过OTA响应中的`api_server`对象配置，与WebSocket使用完全相同的方式：

**OTA响应格式**：
```json
{
  "status": "ok",
  "version": "1.0.0",
  "api_server": {
    "url": "https://api.example.com",
    "token": "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..."
  },
  "websocket": ["url", "token", ...],
  ...
}
```

**字段说明**：
- `url`: API服务器基础URL（不带路径）
- `token`: Bearer token（可选，用于认证）

ESP32会自动：
1. 解析`api_server`对象
2. 将`url`和`token`存储到Settings("api_server")
3. CameraService读取这些配置
4. 上传时构造完整URL：`<url>/api/camera/upload`

**示例配置**：

开发环境：
```json
{
  "api_server": {
    "url": "http://192.168.1.100:5000",
    "token": ""
  }
}
```

生产环境：
```json
{
  "api_server": {
    "url": "https://api.production.com",
    "token": "Bearer prod_token_xxx"
  }
}
```

**配置流程**：
```
ESP32 → OTA请求
   ↓
服务器响应（含api_server对象）
   ↓
ESP32解析并存储到Settings
   ↓
  ├─ WebSocket → Settings("websocket") → 读取url和token
  └─ Camera → Settings("api_server") → 读取url和token
       ↓ 构造上传URL
   ↓
上传到: <api_server.url>/api/camera/upload
```

这种方式的好处：
- **统一配置**：WebSocket和摄像头使用相同的服务器配置
- **简化管理**：只需要配置一次api_server对象
- **环境切换**：OTA响应自动切换开发/生产环境
- **动态配置**：不需要重新编译固件

## 性能优化建议

### 服务端

1. **使用CDN存储图片**
   - 上传后立即转到OSS/S3
   - 返回CDN URL

2. **异步处理**
   - 接收图片后立即返回image_id
   - 人脸/手势检测异步进行

3. **缓存image_id**
   - Redis缓存最近的image_id
   - 设置合理的过期时间

### ESP32端

1. **调整定时间隔**
   - 功耗敏感：10-30秒
   - 响应速度优先：3-5秒

2. **调整JPEG质量**
   - 在Camera配置中降低质量
   - 平衡图片大小和识别精度

3. **增加超时时间**
   - 网络较慢时增加HTTP超时
   - 避免重复上传

## 安全建议

1. **使用HTTPS**
   - 生产环境必须使用HTTPS
   - 防止图片被窃听

2. **添加认证**
   - 配置api_server.token
   - 服务端验证Bearer token

3. **限流**
   - 限制单个设备上传频率
   - 防止资源耗尽

## 参考资料

- [技术设计文档](camera_integration_design.md)
- [Python示例代码](server_example.py)
- [测试脚本](test_camera_upload.sh)
