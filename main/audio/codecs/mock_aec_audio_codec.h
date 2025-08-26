#ifndef MOCK_AEC_AUDIO_CODEC_H
#define MOCK_AEC_AUDIO_CODEC_H

#include "audio/audio_codec.h"

#include <vector>
#include <mutex>

class MockAecAudioCodec : public AudioCodec {
protected:
    std::mutex data_if_mutex_;

public:
    MockAecAudioCodec(int input_sample_rate, int output_sample_rate,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
    gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din);
    ~MockAecAudioCodec();

    
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;

private:
    void InitSimplexChannels(gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                            gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din);
    std::vector<int16_t> ref_buffer_;
    size_t write_pos_ = 0, read_pos_ = 0;
};
#endif // MOCK_AEC_AUDIO_CODEC_H