#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stddef.h>
#include <string.h>
#include <utility>

#include "esp_jpeg_common.h"
#include "esp_jpeg_enc.h"
#include "esp_imgfx_color_convert.h"

#if CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
#include "driver/jpeg_encode.h"
#endif
#include "image_to_jpeg.h"

#define TAG "image_to_jpeg"

static void* malloc_psram(size_t size) {
    void* p = malloc(size);
    if (p)
        return p;
#if (CONFIG_SPIRAM_SUPPORT && (CONFIG_SPIRAM_USE_CAPS_ALLOC || CONFIG_SPIRAM_USE_MALLOC))
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return NULL;
#endif
}

static __always_inline uint8_t expand_5_to_8(uint8_t v) {
    return (uint8_t)((v << 3) | (v >> 2));
}

static __always_inline uint8_t expand_6_to_8(uint8_t v) {
    return (uint8_t)((v << 2) | (v >> 4));
}

static uint8_t* convert_input_to_encoder_buf(const uint8_t* src, uint16_t width, uint16_t height, v4l2_pix_fmt_t format,
                                             jpeg_pixel_format_t* out_fmt, int* out_size) {
    // GRAY 直接作为 JPEG_PIXEL_FORMAT_GRAY 输入
    if (format == V4L2_PIX_FMT_GREY) {
        int sz = (int)width * (int)height;
        uint8_t* buf = (uint8_t*)jpeg_calloc_align(sz, 16);
        if (!buf)
            return NULL;
        memcpy(buf, src, sz);
        if (out_fmt)
            *out_fmt = JPEG_PIXEL_FORMAT_GRAY;
        if (out_size)
            *out_size = sz;
        return buf;
    }

    // V4L2 YUYV (Y Cb Y Cr) 可直接作为 JPEG_PIXEL_FORMAT_YCbYCr 输入
    if (format == V4L2_PIX_FMT_YUYV) {
        int sz = (int)width * (int)height * 2;
        uint8_t* buf = (uint8_t*)jpeg_calloc_align(sz, 16);
        if (!buf)
            return NULL;
        memcpy(buf, src, sz);
        if (out_fmt)
            *out_fmt = JPEG_PIXEL_FORMAT_YCbYCr;
        if (out_size)
            *out_size = sz;
        return buf;
    }

    // V4L2 UYVY (Cb Y Cr Y) -> 重排为 YUYV 再作为 YCbYCr 输入
    // 当前版本暂时不会出现 UYVY 格式
    if (format == V4L2_PIX_FMT_UYVY) [[unlikely]] {
        int sz = (int)width * (int)height * 2;
        const uint8_t* s = src;
        uint8_t* buf = (uint8_t*)jpeg_calloc_align(sz, 16);
        if (!buf)
            return NULL;
        uint8_t* d = buf;
        for (int i = 0; i < sz; i += 4) {
            // src: Cb, Y0, Cr, Y1 -> dst: Y0, Cb, Y1, Cr
            d[0] = s[1];
            d[1] = s[0];
            d[2] = s[3];
            d[3] = s[2];
            s += 4;
            d += 4;
        }
        if (out_fmt)
            *out_fmt = JPEG_PIXEL_FORMAT_YCbYCr;
        if (out_size)
            *out_size = sz;
        return buf;
    }

    // V4L2 YUV422P (YUV422 Planar) -> 重排为 YUYV (YCbYCr)
    // 当前版本暂时不会出现 YUV422P 格式
    if (format == V4L2_PIX_FMT_YUV422P) [[unlikely]] {
        int sz = (int)width * (int)height * 2;
        const uint8_t* y_plane = src;
        const uint8_t* u_plane = y_plane + (int)width * (int)height;
        const uint8_t* v_plane = u_plane + ((int)width / 2) * (int)height;
        uint8_t* buf = (uint8_t*)jpeg_calloc_align(sz, 16);
        if (!buf)
            return NULL;
        uint8_t* dst = buf;
        for (int y = 0; y < height; y++) {
            const uint8_t* y_row = y_plane + y * (int)width;
            const uint8_t* u_row = u_plane + y * ((int)width / 2);
            const uint8_t* v_row = v_plane + y * ((int)width / 2);
            for (int x = 0; x < width; x += 2) {
                uint8_t y0 = y_row[x + 0];
                uint8_t y1 = y_row[x + 1];
                uint8_t cb = u_row[x / 2];
                uint8_t cr = v_row[x / 2];
                dst[0] = y0;
                dst[1] = cb;
                dst[2] = y1;
                dst[3] = cr;
                dst += 4;
            }
        }
        if (out_fmt)
            *out_fmt = JPEG_PIXEL_FORMAT_YCbYCr;
        if (out_size)
            *out_size = sz;
        return buf;
    }

    // RGB 转换为 YUV422 (YCbYCr) 再输入
    // 见 https://github.com/78/xiaozhi-esp32/issues/1380#issuecomment-3497156378
    else if (format == V4L2_PIX_FMT_RGB24 || format == V4L2_PIX_FMT_RGB565 || format == V4L2_PIX_FMT_RGB565X) {
        esp_imgfx_pixel_fmt_t in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB888;
        uint32_t src_len = 0;
        switch (format) {
            case V4L2_PIX_FMT_RGB24:
                in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB888;
                src_len = static_cast<uint32_t>(width * height * 3);
                break;
            case V4L2_PIX_FMT_RGB565:
                in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB565_LE;
                src_len = static_cast<uint32_t>(width * height * 2);
                break;
            [[unlikely]] case V4L2_PIX_FMT_RGB565X: // 当前版本暂时不会出现 RGB565X
                in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB565_BE;
                src_len = static_cast<uint32_t>(width * height * 2);
                break;
            [[unlikely]] default:
                ESP_LOGE(TAG, "[Unreachable Case] unsupported format: 0x%08x", format);
                std::unreachable();
        }
        int sz = (int)width * (int)height * 2;
        uint8_t* buf = (uint8_t*)jpeg_calloc_align(sz, 16);
        if (!buf)
            return nullptr;
        esp_imgfx_color_convert_cfg_t convert_cfg = {
            .in_res = {.width = static_cast<int16_t>(width),
                        .height = static_cast<int16_t>(height)},
            .in_pixel_fmt = in_pixel_fmt,
            .out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_YUYV,
            .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
        };
        esp_imgfx_color_convert_handle_t convert_handle = nullptr;
        esp_imgfx_err_t err = esp_imgfx_color_convert_open(&convert_cfg, &convert_handle);
        if (err != ESP_IMGFX_ERR_OK || convert_handle == nullptr) {
            ESP_LOGE(TAG, "esp_imgfx_color_convert_open failed");
            jpeg_free_align(buf);
            return nullptr;
        }
        esp_imgfx_data_t convert_input_data = {
            .data = const_cast<uint8_t*>(src),
            .data_len = static_cast<uint32_t>(src_len),
        };
        esp_imgfx_data_t convert_output_data = {
            .data = buf,
            .data_len = static_cast<uint32_t>(sz),
        };
        err = esp_imgfx_color_convert_process(convert_handle, &convert_input_data, &convert_output_data);
        if (err != ESP_IMGFX_ERR_OK) {
            ESP_LOGE(TAG, "esp_imgfx_color_convert_process failed");
            jpeg_free_align(buf);
            return nullptr;
        }
        esp_imgfx_color_convert_close(convert_handle);
        convert_handle = nullptr;
        if (out_fmt)
            *out_fmt = JPEG_PIXEL_FORMAT_YCbYCr;
        if (out_size)
            *out_size = sz;
        return buf;
    }
    ESP_LOGE(TAG, "unsupported format: 0x%08x", format);
    if (out_size)
        *out_size = 0;
    return nullptr;
}

#if CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
static jpeg_encoder_handle_t s_hw_jpeg_handle = NULL;

static bool hw_jpeg_ensure_inited(void) {
    if (s_hw_jpeg_handle) {
        return true;
    }
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    esp_err_t er = jpeg_new_encoder_engine(&eng_cfg, &s_hw_jpeg_handle);
    if (er != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %d", (int)er);
        s_hw_jpeg_handle = NULL;
        return false;
    }
    return true;
}

static uint8_t* convert_input_to_hw_encoder_buf(const uint8_t* src, uint16_t width, uint16_t height, v4l2_pix_fmt_t format,
                                                jpeg_enc_input_format_t* out_fmt, int* out_size) {
    if (format == V4L2_PIX_FMT_GREY) {
        int sz = (int)width * (int)height;
        uint8_t* buf = (uint8_t*)malloc_psram(sz);
        if (!buf)
            return NULL;
        memcpy(buf, src, sz);
        if (out_fmt)
            *out_fmt = JPEG_ENCODE_IN_FORMAT_GRAY;
        if (out_size)
            *out_size = sz;
        return buf;
    }

    if (format == V4L2_PIX_FMT_RGB24) {
        int sz = (int)width * (int)height * 3;
        uint8_t* buf = (uint8_t*)malloc_psram(sz);
        if (!buf) {
            ESP_LOGE(TAG, "malloc_psram failed");
            return NULL;
        }
        memcpy(buf, src, sz);
        if (out_fmt)
            *out_fmt = JPEG_ENCODE_IN_FORMAT_RGB888;
        if (out_size)
            *out_size = sz;
        return buf;
    }

    if (format == V4L2_PIX_FMT_RGB565) {
        int sz = (int)width * (int)height * 2;
        uint8_t* buf = (uint8_t*)malloc_psram(sz);
        if (!buf)
            return NULL;
        memcpy(buf, src, sz);
        if (out_fmt)
            *out_fmt = JPEG_ENCODE_IN_FORMAT_RGB565;
        if (out_size)
            *out_size = sz;
        return buf;
    }

    if (format == V4L2_PIX_FMT_YUYV) {
        // 硬件需要 | Y1 V Y0 U | 的“大端”格式，因此需要 bswap16
        int sz = (int)width * (int)height * 2;
        uint16_t* buf = (uint16_t*)malloc_psram(sz);
        if (!buf)
            return NULL;
        const uint16_t* bsrc = (const uint16_t*)src;
        for (int i = 0; i < sz / 2; i++) {
            buf[i] = __builtin_bswap16(bsrc[i]);
        }
        if (out_fmt)
            *out_fmt = JPEG_ENCODE_IN_FORMAT_YUV422;
        if (out_size)
            *out_size = sz;
        return (uint8_t*)buf;
    }

    return NULL;
}

static bool encode_with_hw_jpeg(const uint8_t* src, size_t src_len, uint16_t width, uint16_t height,
                                v4l2_pix_fmt_t format, uint8_t quality, uint8_t** jpg_out, size_t* jpg_out_len,
                                jpg_out_cb cb, void* cb_arg) {
    if (quality < 1)
        quality = 1;
    if (quality > 100)
        quality = 100;

    jpeg_enc_input_format_t enc_src_type = JPEG_ENCODE_IN_FORMAT_RGB888;
    int enc_in_size = 0;
    uint8_t* enc_in = convert_input_to_hw_encoder_buf(src, width, height, format, &enc_src_type, &enc_in_size);
    if (!enc_in) {
        ESP_LOGW(TAG, "hw jpeg: unsupported format, fallback to sw");
        return false;
    }

    if (!hw_jpeg_ensure_inited()) {
        free(enc_in);
        return false;
    }

    jpeg_encode_cfg_t enc_cfg = {0};
    enc_cfg.width = width;
    enc_cfg.height = height;
    enc_cfg.src_type = enc_src_type;
    enc_cfg.image_quality = quality;
    enc_cfg.sub_sample = (enc_src_type == JPEG_ENCODE_IN_FORMAT_GRAY) ? JPEG_DOWN_SAMPLING_GRAY : JPEG_DOWN_SAMPLING_YUV422;

    size_t out_cap = (size_t)width * (size_t)height * 3 / 2 + 64 * 1024;
    if (out_cap < 128 * 1024)
        out_cap = 128 * 1024;
    jpeg_encode_memory_alloc_cfg_t jpeg_enc_output_mem_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t out_cap_aligned = 0;
    uint8_t* outbuf = (uint8_t*)jpeg_alloc_encoder_mem(out_cap, &jpeg_enc_output_mem_cfg, &out_cap_aligned);
    if (!outbuf) {
        free(enc_in);
        ESP_LOGE(TAG, "alloc out buffer failed");
        return false;
    }

    uint32_t out_len = 0;
    esp_err_t er = jpeg_encoder_process(s_hw_jpeg_handle, &enc_cfg, enc_in, (uint32_t)enc_in_size, outbuf, (uint32_t)out_cap_aligned, &out_len);
    free(enc_in);

    if (er != ESP_OK) {
        free(outbuf);
        ESP_LOGE(TAG, "jpeg_encoder_process failed: %d", (int)er);
        return false;
    }

    if (cb) {
        cb(cb_arg, 0, outbuf, (size_t)out_len);
        cb(cb_arg, 1, NULL, 0);
        free(outbuf);
        if (jpg_out)
            *jpg_out = NULL;
        if (jpg_out_len)
            *jpg_out_len = 0;
        return true;
    }

    if (jpg_out && jpg_out_len) {
        *jpg_out = outbuf;
        *jpg_out_len = (size_t)out_len;
        return true;
    }

    free(outbuf);
    return true;
}
#endif // CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER

// 静态变量：保持 JPEG 编码器打开状态，复用内部缓冲区以减少内存碎片化
static jpeg_enc_handle_t g_sw_jpeg_enc_handle = NULL;
static jpeg_enc_config_t g_sw_jpeg_enc_cfg = {};
static bool g_sw_jpeg_enc_inited = false;

// 预分配的输入/输出缓冲区，避免每次拍照都分配/释放大块内存
static uint8_t* g_enc_in_buffer = NULL;
static size_t g_enc_in_buffer_size = 0;
static uint8_t* g_enc_out_buffer = NULL;
static size_t g_enc_out_buffer_size = 0;

// 预分配的颜色转换句柄，避免每次都 open/close
static esp_imgfx_color_convert_handle_t g_color_convert_handle = nullptr;
static bool g_color_convert_inited = false;
static esp_imgfx_color_convert_cfg_t g_color_convert_cfg = {};

static bool encode_with_esp_new_jpeg(const uint8_t* src, size_t src_len, uint16_t width, uint16_t height,
                                     v4l2_pix_fmt_t format, uint8_t quality, uint8_t** jpg_out, size_t* jpg_out_len,
                                     jpg_out_cb cb, void* cb_arg) {
    if (quality < 1)
        quality = 1;
    if (quality > 100)
        quality = 100;

    jpeg_pixel_format_t enc_src_type = JPEG_PIXEL_FORMAT_RGB888;
    int enc_in_size = 0;

    // 根据输入格式设置编码器输入类型
    if (format == V4L2_PIX_FMT_GREY) {
        enc_src_type = JPEG_PIXEL_FORMAT_GRAY;
    } else if (format == V4L2_PIX_FMT_YUYV) {
        enc_src_type = JPEG_PIXEL_FORMAT_YCbYCr;
    } else if (format == V4L2_PIX_FMT_RGB24) {
        enc_src_type = JPEG_PIXEL_FORMAT_RGB888;
    } else if (format == V4L2_PIX_FMT_RGB565 || format == V4L2_PIX_FMT_RGB565X) {
        enc_src_type = JPEG_PIXEL_FORMAT_YCbYCr;  // RGB565 会转换为 YUYV
    }

    // 计算需要的输入缓冲区大小（不分配，只获取大小）
    if (format == V4L2_PIX_FMT_GREY) {
        enc_in_size = width * height;
    } else if (format == V4L2_PIX_FMT_YUYV) {
        enc_in_size = width * height * 2;
    } else if (format == V4L2_PIX_FMT_RGB24) {
        enc_in_size = width * height * 3;
    } else if (format == V4L2_PIX_FMT_RGB565 || format == V4L2_PIX_FMT_RGB565X) {
        enc_in_size = width * height * 2;
    } else {
        ESP_LOGE(TAG, "unsupported format: 0x%08x", format);
        return false;
    }

    // 检查输入缓冲区是否需要重新分配
    if (g_enc_in_buffer == NULL || g_enc_in_buffer_size < (size_t)enc_in_size) {
        if (g_enc_in_buffer != NULL) {
            jpeg_free_align(g_enc_in_buffer);
            g_enc_in_buffer = NULL;
        }
        g_enc_in_buffer = (uint8_t*)jpeg_calloc_align(enc_in_size, 16);
        if (!g_enc_in_buffer) {
            ESP_LOGE(TAG, "alloc input buffer failed: need %d bytes", enc_in_size);
            g_enc_in_buffer_size = 0;
            return false;
        }
        g_enc_in_buffer_size = enc_in_size;
        ESP_LOGI(TAG, "Allocated persistent input buffer: %zu bytes", g_enc_in_buffer_size);
    }

    // 转换输入数据到预分配的缓冲区
    uint8_t* enc_in = g_enc_in_buffer;

    // 执行转换（直接写入预分配的缓冲区）
    if (format == V4L2_PIX_FMT_GREY) {
        memcpy(enc_in, src, enc_in_size);
    } else if (format == V4L2_PIX_FMT_YUYV) {
        memcpy(enc_in, src, enc_in_size);
    } else if (format == V4L2_PIX_FMT_RGB24) {
        memcpy(enc_in, src, enc_in_size);
    } else if (format == V4L2_PIX_FMT_RGB565 || format == V4L2_PIX_FMT_RGB565X) {
        // 对于 RGB565，需要颜色转换到 YUYV
        esp_imgfx_pixel_fmt_t in_pixel_fmt = (format == V4L2_PIX_FMT_RGB565) ?
            ESP_IMGFX_PIXEL_FMT_RGB565_LE : ESP_IMGFX_PIXEL_FMT_RGB565_BE;

        esp_imgfx_color_convert_cfg_t convert_cfg = {
            .in_res = {.width = static_cast<int16_t>(width), .height = static_cast<int16_t>(height)},
            .in_pixel_fmt = in_pixel_fmt,
            .out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_YUYV,
            .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
        };

        // 检查颜色转换句柄是否需要重新打开
        bool need_reopen_convert = !g_color_convert_inited ||
                                   memcmp(&g_color_convert_cfg, &convert_cfg, sizeof(esp_imgfx_color_convert_cfg_t)) != 0;

        if (need_reopen_convert) {
            // 关闭旧的句柄
            if (g_color_convert_handle != nullptr) {
                esp_imgfx_color_convert_close(g_color_convert_handle);
                g_color_convert_handle = nullptr;
                g_color_convert_inited = false;
            }

            // 打开新的句柄
            esp_imgfx_err_t err = esp_imgfx_color_convert_open(&convert_cfg, &g_color_convert_handle);
            if (err != ESP_IMGFX_ERR_OK || g_color_convert_handle == nullptr) {
                ESP_LOGE(TAG, "esp_imgfx_color_convert_open failed");
                return false;
            }

            // 保存配置
            g_color_convert_cfg = convert_cfg;
            g_color_convert_inited = true;
        }

        esp_imgfx_data_t convert_input_data = {
            .data = const_cast<uint8_t*>(src),
            .data_len = static_cast<uint32_t>(enc_in_size),
        };
        esp_imgfx_data_t convert_output_data = {
            .data = enc_in,
            .data_len = static_cast<uint32_t>(enc_in_size),
        };
        esp_imgfx_err_t err = esp_imgfx_color_convert_process(g_color_convert_handle, &convert_input_data, &convert_output_data);

        if (err != ESP_IMGFX_ERR_OK) {
            ESP_LOGE(TAG, "esp_imgfx_color_convert_process failed");
            return false;
        }
    }

    // 构建当前配置
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = width;
    cfg.height = height;
    cfg.src_type = enc_src_type;
    cfg.subsampling = (enc_src_type == JPEG_PIXEL_FORMAT_GRAY) ? JPEG_SUBSAMPLE_GRAY : JPEG_SUBSAMPLE_420;
    cfg.quality = quality;
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.task_enable = false;

    // 检查配置是否变化，如果变化则重新打开编码器
    bool need_reopen = !g_sw_jpeg_enc_inited ||
                       memcmp(&g_sw_jpeg_enc_cfg, &cfg, sizeof(jpeg_enc_config_t)) != 0;

    if (need_reopen) {
        // 关闭旧的编码器
        if (g_sw_jpeg_enc_handle != NULL) {
            jpeg_enc_close(g_sw_jpeg_enc_handle);
            g_sw_jpeg_enc_handle = NULL;
            g_sw_jpeg_enc_inited = false;
        }

        // 打开新的编码器
        jpeg_error_t ret = jpeg_enc_open(&cfg, &g_sw_jpeg_enc_handle);
        if (ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "jpeg_enc_open failed: %d", (int)ret);
            return false;
        }

        // 保存配置
        g_sw_jpeg_enc_cfg = cfg;
        g_sw_jpeg_enc_inited = true;
    }

    // 估算输出缓冲区：宽高的 1.5 倍 + 64KB
    size_t out_cap = (size_t)width * (size_t)height * 3 / 2 + 64 * 1024;
    if (out_cap < 128 * 1024)
        out_cap = 128 * 1024;

    // 检查输出缓冲区是否需要重新分配
    if (g_enc_out_buffer == NULL || g_enc_out_buffer_size < out_cap) {
        if (g_enc_out_buffer != NULL) {
            free(g_enc_out_buffer);
            g_enc_out_buffer = NULL;
        }
        g_enc_out_buffer = (uint8_t*)malloc_psram(out_cap);
        if (!g_enc_out_buffer) {
            ESP_LOGE(TAG, "alloc output buffer failed: need %zu bytes", out_cap);
            g_enc_out_buffer_size = 0;
            return false;
        }
        g_enc_out_buffer_size = out_cap;
        ESP_LOGI(TAG, "Allocated persistent output buffer: %zu bytes", g_enc_out_buffer_size);
    }

    int out_len = 0;
    jpeg_error_t ret = jpeg_enc_process(g_sw_jpeg_enc_handle, enc_in, enc_in_size,
                                         g_enc_out_buffer, (int)g_enc_out_buffer_size, &out_len);

    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_enc_process failed: %d", (int)ret);
        return false;
    }

    if (cb) {
        cb(cb_arg, 0, g_enc_out_buffer, (size_t)out_len);
        cb(cb_arg, 1, NULL, 0);  // 结束信号
        // 注意：不释放 g_enc_out_buffer，保留供下次使用
        if (jpg_out)
            *jpg_out = NULL;
        if (jpg_out_len)
            *jpg_out_len = 0;
        return true;
    }

    if (jpg_out && jpg_out_len) {
        // 调用者需要返回的内存，分配一个新的副本
        *jpg_out = (uint8_t*)malloc_psram(out_len);
        if (!*jpg_out) {
            ESP_LOGE(TAG, "alloc output copy failed: need %d bytes", out_len);
            return false;
        }
        memcpy(*jpg_out, g_enc_out_buffer, out_len);
        *jpg_out_len = (size_t)out_len;
        return true;
    }

    return true;
}

bool image_to_jpeg(uint8_t* src, size_t src_len, uint16_t width, uint16_t height, v4l2_pix_fmt_t format,
                   uint8_t quality, uint8_t** out, size_t* out_len) {
#ifdef CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
    if (format == V4L2_PIX_FMT_JPEG) {
        uint8_t * out_data = (uint8_t*)heap_caps_malloc(src_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for JPEG output");
            return false;
        }
        memcpy(out_data, src, src_len);
        *out = out_data;
        *out_len = src_len;
        return true;
    }
#endif // CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
#if CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
    if (encode_with_hw_jpeg(src, src_len, width, height, format, quality, out, out_len, NULL, NULL)) {
        return true;
    }
    // Fallback to esp_new_jpeg
#endif
    return encode_with_esp_new_jpeg(src, src_len, width, height, format, quality, out, out_len, NULL, NULL);
}

bool image_to_jpeg_cb(uint8_t* src, size_t src_len, uint16_t width, uint16_t height, v4l2_pix_fmt_t format,
                      uint8_t quality, jpg_out_cb cb, void* arg) {
#ifdef CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
    if (format == V4L2_PIX_FMT_JPEG) {
        cb(arg, 0, src, src_len);
        cb(arg, 1, nullptr, 0); // end signal
        return true;
    }
#endif // CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
#if CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
    if (encode_with_hw_jpeg(src, src_len, width, height, format, quality, NULL, NULL, cb, arg)) {
        return true;
    }
    // Fallback to esp_new_jpeg
#endif
    return encode_with_esp_new_jpeg(src, src_len, width, height, format, quality, NULL, NULL, cb, arg);
}
