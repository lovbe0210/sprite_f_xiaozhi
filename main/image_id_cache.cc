#include "image_id_cache.h"
#include "esp_log.h"
#include <algorithm>
#include <esp_timer.h>
#include <mutex>

static const char* TAG = "ImageIdCache";

ImageIdCache::ImageIdCache(size_t max_size) : max_size_(max_size) {
    ESP_LOGI(TAG, "ImageIdCache initialized with max_size=%u", max_size);
}

void ImageIdCache::Add(const ImageIdRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 添加到队列头部
    cache_.push_front(record);

    // 限制大小，删除最旧的记录
    while (cache_.size() > max_size_) {
        cache_.pop_back();
    }

    ESP_LOGD(TAG, "Added record: id=%s, context=%s, cache_size=%u",
             record.id.c_str(), record.context.c_str(), cache_.size());
}

std::string ImageIdCache::GetLatestId(const std::string& context) {
    auto record = GetLatestRecord(context);
    return record.id;
}

ImageIdRecord ImageIdCache::GetLatestRecord(const std::string& context) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找匹配context的最新记录
    for (const auto& record : cache_) {
        if (context.empty() || record.context == context) {
            ESP_LOGD(TAG, "Found record: id=%s, context=%s, requested=%s",
                    record.id.c_str(), record.context.c_str(), context.c_str());
            return record;
        }
    }

    // 未找到，返回空记录
    ESP_LOGD(TAG, "No record found for context=%s", context.c_str());
    return ImageIdRecord{};
}

void ImageIdCache::Cleanup(uint64_t max_age_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t current_time = GetCurrentTimeMs();
    size_t old_size = cache_.size();

    // 移除过期记录
    cache_.erase(
        std::remove_if(cache_.begin(), cache_.end(),
            [current_time, max_age_ms](const ImageIdRecord& record) {
                return record.IsExpired(max_age_ms, current_time);
            }),
        cache_.end()
    );

    size_t removed = old_size - cache_.size();
    if (removed > 0) {
        ESP_LOGI(TAG, "Cleaned up %u expired records, remaining: %u",
                removed, cache_.size());
    }
}

size_t ImageIdCache::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void ImageIdCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t old_size = cache_.size();
    cache_.clear();
    ESP_LOGI(TAG, "Cleared %u records", old_size);
}

void ImageIdCache::SetMaxSize(size_t max_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t old_max = max_size_;
    max_size_ = max_size;

    // 立即执行清理，删除超出的最旧记录
    while (cache_.size() > max_size_) {
        cache_.pop_back();
    }

    ESP_LOGI(TAG, "Max size changed: %u -> %u, cache_size=%u",
            old_max, max_size_, cache_.size());
}

size_t ImageIdCache::GetMaxSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_size_;
}

uint64_t ImageIdCache::GetCurrentTimeMs() const {
    // 使用esp_timer获取高精度时间戳
    return esp_timer_get_time() / 1000;
}
