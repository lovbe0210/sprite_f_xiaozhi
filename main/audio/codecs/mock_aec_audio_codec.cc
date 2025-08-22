#include "mock_aec_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "MockAecAudioCodec"

MockAecAudioCodec::MockAecAudioCodec(int input_sample_rate, int output_sample_rate,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din, bool input_reference) {
    input_reference_ = input_reference;
    input_channels_ = 1 + input_reference; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    
    if (input_reference_) {
        ref_buffer_.resize(960 * 2); // 可根据帧长调整
    }
    InitSimplexChannels(spk_bclk, spk_ws, spk_dout, mic_sck, mic_ws, mic_din);
}

MockAecAudioCodec::~MockAecAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
        i2s_del_channel(rx_handle_);
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
        i2s_del_channel(tx_handle_);
    }
}

void MockAecAudioCodec::InitSimplexChannels(gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                                  gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) {
    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = (i2s_port_t)0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // Create a new channel for MIC
    chan_cfg.id = (i2s_port_t)1;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "MockAEC audio channels created");
}


int MockAecAudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_) {
        return 0;
    }
    
    // 参考no_audio_codec.cc的主体写入逻辑
    std::vector<int32_t> buffer(samples);
    
    // 应用音量控制
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    for (int i = 0; i < samples; i++) {
        int64_t temp = int64_t(data[i]) * volume_factor;
        if (temp > INT32_MAX) {
            buffer[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
            buffer[i] = INT32_MIN;
        } else {
            buffer[i] = static_cast<int32_t>(temp);
        }
    }
    
    // 写入I2S通道
    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    int written_samples = bytes_written / sizeof(int32_t);
    
    // 参考box_audio_codec_lite.cc中对ref_buffer_的处理
    if (input_reference_) {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        if (write_pos_ - read_pos_ + samples > ref_buffer_.size()) {
            assert(ref_buffer_.size() >= samples);
            // 写溢出，只保留最近的数据
            read_pos_ = write_pos_ + samples - ref_buffer_.size();
        }
        
        if (read_pos_) {
            if (write_pos_ != read_pos_) {
                memmove(ref_buffer_.data(), ref_buffer_.data() + read_pos_, (write_pos_ - read_pos_) * sizeof(int16_t));
            }
            write_pos_ -= read_pos_;
            read_pos_ = 0;
        }
        
        // 将数据复制到参考缓冲区
        memcpy(&ref_buffer_[write_pos_], data, samples * sizeof(int16_t));
        write_pos_ += samples;
    }
    
    return written_samples;
}

int MockAecAudioCodec::Read(int16_t* dest, int samples) {
    // 统一计算需要读取的数据大小
    int size = samples / input_channels_;
    int channels = input_channels_ - input_reference_;
    int mic_samples = input_reference_ ? size * channels : samples;
    
    // 统一读取音频数据
    size_t bytes_read;
    std::vector<int32_t> bit32_buffer(mic_samples);
    
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), bit32_buffer.size() * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }
    
    // 转换32位数据为16位数据
    mic_samples = bytes_read / sizeof(int32_t);
    std::vector<int16_t> data(mic_samples);
    for (int i = 0; i < mic_samples; i++) {
        int32_t value = bit32_buffer[i] >> 12;
        data[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
    }
    
    // 有参考输入情况，合并麦克风数据和参考数据
    int j = 0;
    int i = 0;
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    while (i < samples) {
        // mic data
        for (int p = 0; p < channels; p++) {
            dest[i++] = data[j++];
        }
        // ref data
        if (input_reference_) {
            dest[i++] = read_pos_ < write_pos_ ? ref_buffer_[read_pos_++] : 0;       
        } else {
            dest[i++] = 0;
        }
    }

    if (read_pos_ == write_pos_) {
        read_pos_ = write_pos_ = 0;
    }
    
    return samples;
}