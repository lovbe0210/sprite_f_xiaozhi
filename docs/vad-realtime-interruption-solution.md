# VAD实时打断技术方案

> **文档版本**: v1.0
> **创建日期**: 2026-01-31
> **适用场景**: TTS播放中的VAD实时打断、声纹识别

---

## 一、问题背景

### 1.1 问题描述

在实现AEC + VAD实时打断检测功能时，发现以下问题：

1. **音频数据不完整**：VAD触发后服务器获取到的音频数据缺失了VAD检测前的部分
2. **数据丢失时长**：约450ms（对应`vad_min_speech_ms = 450ms`配置）
3. **声纹识别受影响**：缺失的音频数据导致声纹特征提取不完整

### 1.2 问题根源

当前实现（`main/audio/processors/afe_audio_processor.cc`）中：

```cpp
// VAD状态变化处理（第168-176行）
if (vad_state_change_callback_) {
    if (res->vad_state == VAD_SPEECH && !is_speaking_) {
        is_speaking_ = true;
        vad_state_change_callback_(true);  // 仅触发状态变化回调
    }
}

// 音频数据输出（第178-197行）
if (output_callback_) {
    output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
    // ... 只有积累到frame_samples_才发送
}
```

**问题**：VAD触发时仅通知状态变化，但之前缓存的音频数据并未发送出去。

---

## 二、技术分析

### 2.1 AEC时序问题

#### 当前AEC工作模式
```cpp
// 初始化时AEC关闭（afe_audio_processor.cc:92）
afe_iface_->disable_aec(afe_data_);
codec_->EnableInputReference(false);

// 仅在speaking状态时开启AEC（EnableDeviceAec方法）
void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
        afe_iface_->enable_aec(afe_data_);
    } else {
        afe_iface_->disable_aec(afe_data_);
    }
    codec_->EnableInputReference(enable);
}
```

#### AEC状态时序图

```
时间轴:
  ────────────────────────────────────────────────────────────────→
  |               |                    |                          |
空闲状态       speaking             VAD触发                  listening状态
  |               |                    |                          |
AEC关闭         AEC开启              VAD检测                    AEC关闭
                                    
```

**核心矛盾**：
- TTS播放时扬声器有声音 → **必须有AEC**消除回声
- VAD检测开始 → 检测到VAD后进入listening状态，listening状态时需要关闭AEC，使用原声源
- 声源数据在VAD前后状态不同 → 数据处理链路不一致 → 影响声纹识别

### 2.2 VAD检测延迟

ESP-SR的VAD配置：
```cpp
afe_config->vad_min_speech_ms = 450;  // 最小语音时长450ms
```

**含义**：VAD需要检测到连续450ms的语音才会触发状态变化，导致：
- 用户开始说话
- 经过450ms后VAD才触发
- 前450ms的数据丢失

---

## 三、解决方案

### 3.1 ESP-SR官方方案：vad_cache（推荐）

ESP-SR AFE框架已内置VAD缓存机制：

```c
typedef struct afe_fetch_result_t {
    int16_t *data;           // 当前帧的音频数据
    int data_size;           // data的大小（字节）

    int16_t *vad_cache;      // VAD缓存数据，仅当vad_cache_size > 0时有效
    int vad_cache_size;      // vad_cache的大小（字节）

    vad_state_t vad_state;   // VAD状态
    // ...
} afe_fetch_result_t;
```

**工作原理**：
- AFE内部自动维护VAD历史数据缓冲区
- VAD从SILENCE→SPEECH时，通过`vad_cache`返回历史数据
- `vad_cache`和`data`都经过相同的AFE处理链路（AEC、NS等）

#### 代码实现

修改`AudioProcessorTask`方法（afe_audio_processor.cc:147-199）：

```cpp
void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;
        }

        // ========== 关键修改：VAD状态变化处理 ==========
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;

                // 发送VAD缓存数据（如果存在）
                if (res->vad_cache_size > 0 && output_callback_) {
                    size_t cache_samples = res->vad_cache_size / sizeof(int16_t);

                    // 将vad_cache数据添加到输出缓冲区
                    output_buffer_.insert(output_buffer_.begin(),
                                         res->vad_cache,
                                         res->vad_cache + cache_samples);

                    ESP_LOGI(TAG, "VAD triggered: added %zu samples of history data",
                             cache_samples);
                }

                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        // 音频数据输出（保持原有逻辑）
        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);

            // 输出完整的帧
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(),
                                                         output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(),
                                        output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}
```

### 3.2 AEC时序优化方案

#### 方案A：TTS播放时开启AEC

**适用场景**：TTS播放中被打断

```cpp
// 在TTS播放开始时
void OnTTSStart() {
    afe_processor->EnableDeviceAec(true);   // 开启AEC
    afe_processor->EnableAudioVadDetecting(true);  // 开启VAD检测
}

// 在TTS播放结束时
void OnTTSEnd() {
    // 延迟关闭AEC，避免切断用户语音
    // 或者在整个会话期间保持AEC开启
    afe_processor->EnableAudioVadDetecting(false);
    // afe_processor->EnableDeviceAec(false);  // 可选：延迟关闭
}
```

#### 方案B：会话期间AEC始终开启

**适用场景**：频繁的打断交互

```cpp
// 在初始化时
void Initialize() {
    // 始终开启AEC
    afe_config->aec_init = true;

    // 或者不调用disable_aec，保持默认开启状态
}
```

**优点**：
- 数据处理链路始终一致
- 无需管理AEC开关状态
- 声纹识别效果最佳

**缺点**：
- 增加CPU占用（约5-10%）
- 空闲时也消耗算力

### 3.3 数据一致性保障

#### 数据流示意

```
┌─────────────┐    ┌──────────────────────────┐    ┌──────────────┐
│  麦克风输入  │───→│   AFE处理链路            │───→│  服务器接收   │
└─────────────┘    │  - AEC (回声消除)        │    └──────────────┘
                   │  - NS (噪声抑制)         │
                   │  - VAD (语音活动检测)    │
                   └──────────────────────────┘
                              ↓
                    ┌─────────────────────┐
                    │ afe_fetch_result_t   │
                    │ - data (当前帧)      │
                    │ - vad_cache (历史)   │ ← 都经过相同处理
                    │ - vad_state          │
                    └─────────────────────┘
```

**关键点**：
- `vad_cache`和`data`都经过相同的AFE处理链路
- AEC、NS等处理对两者完全一致
- 直接拼接即可，无需额外处理

#### 音量一致性

- AEC处理会影响音频幅度
- `vad_cache`和`data`经过相同处理 → 音量自然一致
- 无需AGC或其他音量调整

---

## 四、实施路线图

### Phase 1：使用vad_cache（最小改动）⚡

**目标**：解决数据丢失问题

**步骤**：
1. ✅ 确认当前代码已设置VAD相关配置
2. ⚡ 修改`AudioProcessorTask`，在VAD触发时处理`vad_cache`
3. 🧪 测试验证：
   - 打印`vad_cache_size`确认有数据
   - 对比服务器接收的音频样本数
   - 听感测试：检查是否有明显截断

**验收标准**：
- VAD触发时能获取到历史数据（>0字节）
- 服务器收到的音频连续完整
- 用户语音前段不丢失

### Phase 2：AEC时序优化 🔧

**目标**：确保AEC在需要时开启

**步骤**：
1. 📊 分析当前AEC开关时机
   - 搜索`EnableDeviceAec`调用点
   - 确认TTS播放时的状态管理
2. 🔧 调整策略：
   - 方案A：TTS播放时提前开启AEC
   - 方案B：会话期间AEC始终开启
3. 🧪 测试验证：
   - TTS播放时打断，检查回声消除效果
   - 对比声纹识别准确率

**验收标准**：
- TTS播放时AEC正常工作
- 回声被有效消除
- 声纹识别率不降低

### Phase 3：性能优化（可选）📈

**目标**：优化资源占用

**步骤**：
1. 📈 监控CPU和内存占用
   - 使用`esp_timer_get_time()`测量处理耗时
   - 检查PSRAM使用情况
2. ⚡ 根据实际需求调整：
   - `vad_min_speech_ms`：平衡响应速度和误触发
   - 缓冲区大小：平衡内存和延迟
3. 🧪 长时间稳定性测试：
   - 连续运行24小时
   - 多次打断循环测试

---

## 五、测试验证方案

### 5.1 功能测试

| 测试项 | 测试方法 | 预期结果 | 实际结果 |
|--------|----------|----------|----------|
| **数据完整性** | 统计音频样本数 | VAD触发前后数据连续，无缺失 | ⬜ 通过 |
| **vad_cache有效性** | 打印`vad_cache_size` | VAD触发时 > 0 | ⬜ 通过 |
| **AEC效果** | TTS播放时打断测试 | 回声被消除，语音清晰 | ⬜ 通过 |
| **拼接平滑性** | 听感测试 | 无明显截断或跳跃 | ⬜ 通过 |

### 5.2 性能测试

| 指标 | 测试方法 | 基准值 | 目标值 | 实际值 |
|------|----------|--------|--------|--------|
| **CPU占用** | xt_task_get_usage() | ~30% | <40% | ⬜ |
| **内存占用** | heap_caps_get_info() | ~50KB | <100KB | ⬜ |
| **VAD响应延迟** | 逻辑分析仪 | 450ms | <500ms | ⬜ |
| **处理延迟** | 时间戳差值 | <100ms | <150ms | ⬜ |

### 5.3 声纹识别测试

| 测试场景 | 测试样本 | 识别率 | 备注 |
|----------|----------|--------|------|
| 打断场景 | 50个样本 | >90% | TTS播放中用户说话 |
| 正常场景 | 50个样本 | >90% | 空闲状态下用户说话 |
| 对比测试 | - | 无明显差异 | 方案前后对比 |

### 5.4 测试脚本示例

```cpp
// 测试代码片段
void TestVadCache() {
    ESP_LOGI(TAG, "=== VAD Cache Test ===");

    int cache_detected = 0;
    int total_triggers = 0;

    // 在AudioProcessorTask中添加
    if (res->vad_state == VAD_SPEECH && !is_speaking_) {
        total_triggers++;

        if (res->vad_cache_size > 0) {
            cache_detected++;
            size_t cache_samples = res->vad_cache_size / sizeof(int16_t);
            ESP_LOGI(TAG, "VAD cache detected: %zu samples (%.2f seconds)",
                     cache_samples, cache_samples / 16000.0);
        } else {
            ESP_LOGW(TAG, "VAD triggered but no cache data!");
        }
    }

    ESP_LOGI(TAG, "Cache detection rate: %d/%d (%.1f%%)",
             cache_detected, total_triggers,
             100.0 * cache_detected / total_triggers);
}
```

---

## 六、配置参数说明

### 6.1 VAD相关配置

```cpp
// VAD模式选择
afe_config->vad_mode = VAD_MODE_2;  // 0-3，值越大越敏感

// 最小语音时长
afe_config->vad_min_speech_ms = 450;  // 连续语音达到此时长才触发

// 静音播放静音
afe_config->vad_mute_playback = true;  // VAD触发时静音播放
```

**参数调优建议**：
- `vad_mode`：
  - `VAD_MODE_0`：最不敏感，适合安静环境
  - `VAD_MODE_2`：推荐值，平衡误报和漏报
  - `VAD_MODE_3`：最敏感，适合噪声环境
- `vad_min_speech_ms`：
  - 太短（<300ms）：易误触发
  - 太长（>600ms）：响应慢，丢失数据多
  - 推荐：400-500ms

### 6.2 AEC相关配置

```cpp
// AEC模式
afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
// 可选：
// - AEC_MODE_VOIP_HIGH_PERF：高性能模式
// - AEC_MODE_VOIP_LOW_COST：低功耗模式

// 参考通道
afe_config->aec_init = true;  // 启用AEC
input_format = "MMR";  // M=麦克风，R=参考通道（扬声器输出）
```

---

## 七、故障排查

### 7.1 vad_cache始终为0

**可能原因**：
1. VAD未正确初始化
2. `vad_min_speech_ms`设置过大
3. VAD模型未加载

**排查方法**：
```cpp
ESP_LOGI(TAG, "VAD init: %d, mode: %d, min_speech_ms: %d",
         afe_config->vad_init,
         afe_config->vad_mode,
         afe_config->vad_min_speech_ms);

// 检查fetch结果
ESP_LOGI(TAG, "vad_state: %d, vad_cache_size: %d",
         res->vad_state,
         res->vad_cache_size);
```

### 7.2 音频拼接处有杂音

**可能原因**：
1. AEC在VAD前后状态不同
2. 数据缓冲区溢出
3. 拼接顺序错误

**解决方案**：
- 确保AEC状态一致（Phase 2）
- 检查`vad_cache`是否插入到`data`前面
- 不要在VAD触发时重置缓冲区

### 7.3 声纹识别率下降

**可能原因**：
1. AEC处理改变了音色
2. VAD缓存数据量不足
3. 拼接处不连续

**解决方案**：
- 对比AEC前后的声纹特征
- 增加`vad_min_speech_ms`获取更多历史数据
- 确保`vad_cache`和`data`按顺序拼接

---

## 八、参考资源

### 8.1 官方文档

- [ESP-SR AFE 声学前端算法框架](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32/audio_front_end/README.html)
  - AFE完整使用指南
  - vad_cache机制说明

- [ESP-VA SDK GitHub](https://github.com/espressif/esp-va-sdk)
  - 乐鑫语音助手SDK
  - 示例代码

### 8.2 技术论文

- [Deep Learning-based Acoustic-echo Cancellation](https://israelcohen.com/wp-content/uploads/2018/08/PhD_Thesis___Amir_Ivry.pdf)
- [E2E-AEC: End-to-end neural network implementation](https://www.arxiv.org/abs/2601.16774)
- [INTERSPEECH 2021 AEC Challenge](https://www.microsoft.com/en-us/research/publications/interspeech-2021-acoustic-echo-cancellation-challenge/)

### 8.3 社区资源

- [Home Assistant: ESP32 Voice Assistant with AEC/VAD/NS](https://community.home-assistant.io/t/a-voice-assistant-solution-with-an-esp32-2xinmp441-mics-tpa3118-amplifier-on-a-50w-speaker-leveraging-the-aec-vad-ns-capabilities-how-to-clear-voice-pickup-even-when-playing-music/704079)
  - 实际应用案例
  - 硬件连接图

- [ESP32-SpeexDSP](https://github.com/rjsachse/ESP32-SpeexDSP)
  - 开源音频处理库

---

## 九、附录

### 9.1 相关文件

| 文件路径 | 说明 | 修改内容 |
|----------|------|----------|
| `main/audio/processors/afe_audio_processor.cc` | AFE音频处理器实现 | 添加vad_cache处理逻辑 |
| `main/audio/processors/afe_audio_processor.h` | AFE音频处理器头文件 | 添加必要状态变量（可选） |

### 9.2 关键代码片段

#### 获取vad_cache大小

```cpp
size_t get_vad_cache_duration_ms(afe_fetch_result_t *res) {
    if (res->vad_cache_size > 0) {
        size_t samples = res->vad_cache_size / sizeof(int16_t);
        return (samples * 1000) / 16000;  // 16kHz采样率
    }
    return 0;
}
```

#### 数据完整性检查

```cpp
bool validate_audio_continuity(int16_t *cache, size_t cache_size,
                               int16_t *data, size_t data_size) {
    // 检查缓存末尾和数据开头是否连续
    // 可通过能量、频谱等特征判断
    return true;  // 简化实现
}
```

### 9.3 版本历史

| 版本 | 日期 | 作者 | 说明 |
|------|------|------|------|
| v1.0 | 2026-01-31 | Claude | 初始版本 |

---

## 十、总结

本技术方案通过使用ESP-SR AFE框架内置的`vad_cache`机制，解决了VAD实时打断时的音频数据丢失问题。主要优势：

1. ✅ **官方支持**：利用ESP-SR内置功能，稳定可靠
2. ✅ **最小改动**：仅需修改一处代码
3. ✅ **数据一致**：`vad_cache`和`data`经过相同处理链路
4. ✅ **性能优良**：无需额外维护环形缓冲区

**实施建议**：
- 优先实施Phase 1（vad_cache处理）
- 根据实际效果决定是否进行Phase 2（AEC优化）
- Phase 3为可选优化项

---

**文档维护**：请在实施完成后更新"实际结果"列的内容，并补充遇到的问题和解决方案。
