#include "afe_audio_processor.h"
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    // 参考通道使用模拟缓冲音频    
    #ifdef CONFIG_USE_DEVICE_AEC
        int ref_num = 1;
    #else
        int ref_num = 0;
    #endif

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);

    // 噪音抑制
    #ifdef CONFIG_USE_AUDIO_NOISE_SUPPRESSION
        char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
        if (ns_model_name != nullptr) {
            afe_config->ns_init = true;
            afe_config->ns_model_name = ns_model_name;
            afe_config->afe_ns_mode = AFE_NS_MODE_NET;
        } else {
            afe_config->ns_init = false;
        }
    #else
        afe_config->ns_init = false;
    #endif

    // VAD检测
    #ifdef CONFIG_USE_AUDIO_VAD_DETECTION
        char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
        if (vad_model_name != nullptr) {
            afe_config->vad_init = true;
            afe_config->vad_model_name = vad_model_name;
            afe_config->vad_mode = VAD_MODE_2;
            afe_config->vad_min_speech_ms = 450;
            afe_config->vad_mute_playback = true;
        } else {
            afe_config->vad_init = false;
        }
    #else
        afe_config->vad_init = false;
    #endif

    // 设备端回声消除    
    #ifdef CONFIG_USE_DEVICE_AEC
        afe_config->aec_init = true;
        afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    #else
        afe_config->aec_init = false;
    #endif

    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_config_print(afe_config);
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    // 关闭AEC功能，只在speaking状态下开启
    #ifdef CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_aec(afe_data_);
        codec_->EnableInputReference(false);
    #endif
    
    xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, 4, NULL);
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);

                // 将VAD缓存数据添加到输出缓冲区前端，解决开头数据丢失问题
                if (res->vad_cache_size > 0 && output_callback_) {
                    size_t cache_samples = res->vad_cache_size / sizeof(int16_t);

                    // 清空 output_buffer_，确保只有 vad_cache + 当前 data
                    if (!output_buffer_.empty()) {
                        output_buffer_.clear();
                        output_buffer_.reserve(cache_samples + frame_samples_);
                    }

                    // 对VAD缓存数据进行增益补偿，使其接近原始麦克风数据
                    // MockAecAudioCodec在input_reference=true时使用>>8然后/256（相当于>>16）
                    // 在input_reference=false时使用>>12，差异为16倍（2^16 / 2^12 = 16）
                    // 所以需要乘以16来补偿增益差异
                    std::vector<int16_t> compensated_cache(cache_samples);
                    for (size_t i = 0; i < cache_samples; i++) {
                        int32_t temp = (int32_t)res->vad_cache[i] * 16;
                        // 限制在int16范围内防止溢出
                        compensated_cache[i] = (temp > INT16_MAX) ? INT16_MAX :
                                              (temp < INT16_MIN) ? INT16_MIN :
                                              (int16_t)temp;
                    }

                    // 将补偿后的vad_cache数据插入到output_buffer_的前面
                    output_buffer_.insert(output_buffer_.begin(),
                                         compensated_cache.begin(),
                                         compensated_cache.end());
                }
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);

            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        } 
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    #if CONFIG_USE_DEVICE_AEC
        if (enable) {
            afe_iface_->enable_aec(afe_data_);
        } else {
            afe_iface_->disable_aec(afe_data_);
        }
        codec_->EnableInputReference(enable);
    #else
        ESP_LOGE(TAG, "Device AEC is not supported");
    #endif
}

void AfeAudioProcessor::EnableAudioVadDetecting(bool enable) {
    if (enable) {
        afe_iface_->enable_vad(afe_data_);
    } else {
        afe_iface_->disable_vad(afe_data_);
    }
}
