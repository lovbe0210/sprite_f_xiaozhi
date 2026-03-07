#ifndef _IMAGE_ID_CACHE_H_
#define _IMAGE_ID_CACHE_H_

#include <string>
#include <deque>
#include <mutex>
#include <cstdint>

/**
 * @brief 图片ID记录
 *
 * 用于存储从服务端返回的图片ID及其元数据
 */
struct ImageIdRecord {
    std::string id;                    ///< 服务端返回的image_id
    uint64_t timestamp_ms;             ///< 时间戳（毫秒）
    std::string context;               ///< "active", "listening", "speaking"
    std::string url;                   ///< CDN URL（可选）

    ImageIdRecord() : timestamp_ms(0) {}

    ImageIdRecord(const std::string& id_, uint64_t timestamp_ms_,
                 const std::string& context_, const std::string& url_ = "")
        : id(id_), timestamp_ms(timestamp_ms_), context(context_), url(url_) {}

    /**
     * @brief 判断记录是否过期
     * @param max_age_ms 最大存活时间（毫秒）
     * @return true 如果已过期
     */
    bool IsExpired(uint64_t max_age_ms, uint64_t current_time_ms) const {
        if (timestamp_ms == 0) {
            return true;
        }
        return (current_time_ms - timestamp_ms) > max_age_ms;
    }
};

/**
 * @brief 图片ID缓存队列
 *
 * 用于缓存服务端返回的图片ID，提供线程安全的访问接口
 *
 * 功能特点：
 * - 线程安全：使用mutex保护内部数据
 * - 自动过期：支持清理过期记录
 * - 按context过滤：可获取指定context的最新ID
 * - 大小限制：自动清理最旧的记录
 */
class ImageIdCache {
public:
    /**
     * @brief 构造函数
     * @param max_size 最大缓存数量，默认20
     */
    explicit ImageIdCache(size_t max_size = 20);

    ~ImageIdCache() = default;

    /**
     * @brief 添加新的image_id记录
     * @param record 图片ID记录
     *
     * 线程安全，会自动限制队列大小
     */
    void Add(const ImageIdRecord& record);

    /**
     * @brief 获取最新的image_id（按context过滤）
     * @param context 上下文类型，如"active", "listening", "speaking"
     *               如果为空，则返回任意context的最新ID
     * @return 最新图片ID，如果未找到返回空字符串
     */
    std::string GetLatestId(const std::string& context = "");

    /**
     * @brief 获取最新的完整记录（按context过滤）
     * @param context 上下文类型，如果为空则返回任意context的最新记录
     * @return 最新记录，如果未找到返回空记录
     */
    ImageIdRecord GetLatestRecord(const std::string& context = "");

    /**
     * @brief 清理过期记录
     * @param max_age_ms 最大存活时间（毫秒），默认60秒
     *
     * 线程安全，会移除所有过期的记录
     */
    void Cleanup(uint64_t max_age_ms = 60000);

    /**
     * @brief 获取当前缓存大小
     * @return 缓存中的记录数量
     */
    size_t Size() const;

    /**
     * @brief 清空所有记录
     */
    void Clear();

    /**
     * @brief 设置最大缓存数量
     * @param max_size 最大数量
     *
     * 设置后会立即执行清理，删除超出的最旧记录
     */
    void SetMaxSize(size_t max_size);

    /**
     * @brief 获取最大缓存数量
     * @return 最大缓存数量
     */
    size_t GetMaxSize() const;

private:
    mutable std::mutex mutex_;
    std::deque<ImageIdRecord> cache_;
    size_t max_size_;

    /**
     * @brief 获取当前时间戳（毫秒）
     * @return 当前时间戳
     */
    uint64_t GetCurrentTimeMs() const;
};

#endif  // _IMAGE_ID_CACHE_H_
