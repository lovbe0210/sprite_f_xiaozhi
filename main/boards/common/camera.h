#ifndef CAMERA_H
#define CAMERA_H

#include <string>
#include <cstdint>

/**
 * @brief 帧数据结构
 */
struct FrameData {
    const uint8_t* data = nullptr;  // 数据指针
    size_t len = 0;                 // 数据长度
    uint16_t width = 0;             // 图像宽度
    uint16_t height = 0;            // 图像高度
    uint32_t format = 0;            // 像素格式 (V4L2_PIX_FMT_*)
};

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool CaptureRawFrame() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual std::string Explain(const std::string& question) = 0;

    /**
     * @brief 获取当前帧数据
     * @return FrameData 结构，包含数据指针和长度
     * @note 数据指针只在下一次 CaptureRawFrame() 调用前有效
     */
    virtual FrameData GetFrameData() = 0;

    /**
     * @brief 释放帧数据（可选，某些实现可能需要）
     */
    virtual void ReleaseFrameData() = 0;
};

#endif // CAMERA_H
