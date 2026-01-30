#include "mock_aec_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "MockAecAudioCodec"

MockAecAudioCodec::MockAecAudioCodec(int input_sample_rate, int output_sample_rate,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    input_reference_ = true; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数

    time_us_write_ = 0;
    time_us_read_ = 0;
    slice_index_ = 0;

    // 判断是否需要重采样：AEC要求参考信号必须是16kHz
    needs_resample_ = (output_sample_rate_ != 16000);
    if (needs_resample_) {
        ref_resampler_.Configure(output_sample_rate_, 16000);
        ESP_LOGI(TAG, "Reference resampler initialized: %dHz -> 16000Hz", output_sample_rate_);
    }

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
    ESP_LOGI(TAG, "Simplex channels created");
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
    const int32_t play_size = 512;
    std::vector<int32_t> buffer(samples);

    // output_volume_: 0-100
    // volume_factor_: 0-65536
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (output_buffer_.size() < play_size*10) {
            output_buffer_.resize(play_size*10,0);
            slice_index_ = 0;
        }

        // 扬声器输出：始终使用音量控制后的数据
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

        // 确定要写入参考缓冲区的数据
        const int16_t* ref_data;
        int ref_samples;

        std::vector<int16_t> volume_adjusted_data(samples);
        for (int i = 0; i < samples; i++) {
            int64_t temp = int64_t(data[i]) * volume_factor;
            volume_adjusted_data[i] = (temp > INT32_MAX) ? INT16_MAX :
                                        (temp < INT32_MIN) ? INT16_MIN :
                                        static_cast<int16_t>(temp >> 16);
        }

        if (needs_resample_) {
            ref_samples = ref_resampler_.GetOutputSamples(samples);
            resampled_buffer_.resize(ref_samples);
            ref_resampler_.Process(volume_adjusted_data.data(), samples, resampled_buffer_.data());
            ref_data = resampled_buffer_.data();
        } else {
            ref_data = volume_adjusted_data.data();
            ref_samples = samples;
        }

        for (int i = 0; i < ref_samples; i++) {
            output_buffer_[slice_index_] = ref_data[i];
            slice_index_++;
            if(slice_index_ >= play_size*10) slice_index_ = 0;
        }

        time_us_write_ = esp_timer_get_time(); // 获取微秒级时间戳
    }
    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    return bytes_written / sizeof(int32_t);
}

int MockAecAudioCodec::Read(int16_t* dest, int samples) {
    static int32_t i_index = 0;
    static bool first_speak = true;
    const int32_t play_size = 512;

    // 如果没有启用参考音频输入，直接读取麦克风原始数据
    // 这样数据质量恒定，便于服务器做声纹识别
    if (!input_reference_) {
        size_t bytes_read;
        std::vector<int32_t> bit32_buffer(samples / 2);
        if (i2s_channel_read(rx_handle_, bit32_buffer.data(), samples / 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "Read Failed!");
            return 0;
        }

        int read_samples = bytes_read / sizeof(int32_t);
        for (int i = 0; i < read_samples; i++) {
            int32_t value = bit32_buffer[i] >> 8;
            int64_t temp = int64_t(value) / 256;
            // 麦克风原始数据
            dest[i * 2] = (temp > INT16_MAX) ? INT16_MAX : (temp < INT16_MIN) ? INT16_MIN : static_cast<int16_t>(temp);
            // 参考音频位置填充0
            dest[i * 2 + 1] = 0;
        }
        return read_samples * 2;
    }

    // 启用参考音频输入时，进行AEC处理和打断检测
    {
        std::unique_lock<std::mutex> lock(mutex_);
        time_us_read_ = esp_timer_get_time(); // 获取微秒级时间戳
        if (time_us_read_ - time_us_write_ > 1000 * 100) { // 100ms
            std::fill(output_buffer_.begin(), output_buffer_.end(), 0);
            first_speak = true;
            slice_index_ = 0;
            i_index = play_size * 10 - 512;
        } else {
            if (first_speak) {
                first_speak = false;
                i_index = 0;
            }
        }
        if (i_index < 0) i_index = play_size * 10 + i_index;
    }

    size_t bytes_read;
    std::vector<int32_t> bit32_buffer(samples / 2);
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), samples / 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; i++) {
        int32_t value = bit32_buffer[i] >> 8;
        int64_t temp = int64_t(value) / 256; // 使用 int64_t 进行乘法运算
        // 目前只考虑单麦克风的设计，一个麦克风一个参考音频，交错填充数据
        dest[i * 2] = (temp > INT16_MAX) ? INT16_MAX : (temp < -INT16_MAX) ? -INT16_MAX : static_cast<int16_t>(temp);
        if (output_buffer_.size() > i_index) {
            dest[i * 2 + 1] = output_buffer_[i_index];
        } else {
            dest[i * 2 + 1] = 0;
        }
        i_index++;
        if (i_index >= play_size * 10) i_index = i_index - play_size * 10;
    }
    return samples * 2;
}