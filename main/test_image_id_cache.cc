#include "image_id_cache.h"
#include "esp_log.h"
#include <thread>
#include <chrono>

static const char* TAG = "TestImageIdCache";

/**
 * @brief ImageIdCache 单元测试
 *
 * 测试内容：
 * 1. 基本功能测试（添加、获取）
 * 2. 线程安全测试
 * 3. 过期清理测试
 * 4. 大小限制测试
 * 5. Context过滤测试
 */
namespace TestImageIdCache {

// 测试1：基本功能测试
bool TestBasicOperations() {
    ESP_LOGI(TAG, "=== Test 1: Basic Operations ===");

    ImageIdCache cache(5);

    // 添加记录
    cache.Add(ImageIdRecord{"img_001", 1000, "active"});
    cache.Add(ImageIdRecord{"img_002", 2000, "listening"});
    cache.Add(ImageIdRecord{"img_003", 3000, "speaking"});

    // 获取最新ID
    std::string latest = cache.GetLatestId();
    if (latest != "img_001") {
        ESP_LOGE(TAG, "Test 1.1 FAILED: expected 'img_001', got '%s'", latest.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Test 1.1 PASSED: GetLatestId() = '%s'", latest.c_str());

    // 按context获取
    std::string listening_id = cache.GetLatestId("listening");
    if (listening_id != "img_002") {
        ESP_LOGE(TAG, "Test 1.2 FAILED: expected 'img_002', got '%s'", listening_id.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Test 1.2 PASSED: GetLatestId('listening') = '%s'", listening_id.c_str());

    // 获取大小
    size_t size = cache.Size();
    if (size != 3) {
        ESP_LOGE(TAG, "Test 1.3 FAILED: expected size 3, got %u", size);
        return false;
    }
    ESP_LOGI(TAG, "Test 1.3 PASSED: Size() = %u", size);

    ESP_LOGI(TAG, "=== Test 1: PASSED ===\n");
    return true;
}

// 测试2：大小限制测试
bool TestSizeLimit() {
    ESP_LOGI(TAG, "=== Test 2: Size Limit ===");

    ImageIdCache cache(3);  // 最大3条

    // 添加5条记录
    cache.Add(ImageIdRecord{"img_001", 1000, "active"});
    cache.Add(ImageIdRecord{"img_002", 2000, "active"});
    cache.Add(ImageIdRecord{"img_003", 3000, "active"});
    cache.Add(ImageIdRecord{"img_004", 4000, "active"});
    cache.Add(ImageIdRecord{"img_005", 5000, "active"});

    // 应该只保留最新的3条
    size_t size = cache.Size();
    if (size != 3) {
        ESP_LOGE(TAG, "Test 2.1 FAILED: expected size 3, got %u", size);
        return false;
    }
    ESP_LOGI(TAG, "Test 2.1 PASSED: Size limited to 3");

    // 最新的应该是img_005
    std::string latest = cache.GetLatestId();
    if (latest != "img_005") {
        ESP_LOGE(TAG, "Test 2.2 FAILED: expected 'img_005', got '%s'", latest.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Test 2.2 PASSED: Latest record is 'img_005'");

    ESP_LOGI(TAG, "=== Test 2: PASSED ===\n");
    return true;
}

// 测试3：Context过滤测试
bool TestContextFilter() {
    ESP_LOGI(TAG, "=== Test 3: Context Filter ===");

    ImageIdCache cache(10);

    // 添加不同context的记录
    cache.Add(ImageIdRecord{"img_active_001", 1000, "active"});
    cache.Add(ImageIdRecord{"img_listening_001", 2000, "listening"});
    cache.Add(ImageIdRecord{"img_active_002", 3000, "active"});
    cache.Add(ImageIdRecord{"img_speaking_001", 4000, "speaking"});
    cache.Add(ImageIdRecord{"img_listening_002", 5000, "listening"});

    // 获取最新的active
    std::string active_id = cache.GetLatestId("active");
    if (active_id != "img_active_002") {
        ESP_LOGE(TAG, "Test 3.1 FAILED: expected 'img_active_002', got '%s'", active_id.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Test 3.1 PASSED: GetLatestId('active') = '%s'", active_id.c_str());

    // 获取最新的listening
    std::string listening_id = cache.GetLatestId("listening");
    if (listening_id != "img_listening_002") {
        ESP_LOGE(TAG, "Test 3.2 FAILED: expected 'img_listening_002', got '%s'", listening_id.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Test 3.2 PASSED: GetLatestId('listening') = '%s'", listening_id.c_str());

    // 获取speaking
    std::string speaking_id = cache.GetLatestId("speaking");
    if (speaking_id != "img_speaking_001") {
        ESP_LOGE(TAG, "Test 3.3 FAILED: expected 'img_speaking_001', got '%s'", speaking_id.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Test 3.3 PASSED: GetLatestId('speaking') = '%s'", speaking_id.c_str());

    ESP_LOGI(TAG, "=== Test 3: PASSED ===\n");
    return true;
}

// 测试4：过期清理测试
bool TestExpiration() {
    ESP_LOGI(TAG, "=== Test 4: Expiration Cleanup ===");

    ImageIdCache cache(10);

    // 获取当前时间
    uint64_t now = esp_timer_get_time() / 1000;

    // 添加过期记录（100秒前）
    cache.Add(ImageIdRecord{"img_old_001", now - 100000, "active"});

    // 添加新记录（1秒前）
    cache.Add(ImageIdRecord{"img_new_001", now - 1000, "listening"});
    cache.Add(ImageIdRecord{"img_new_002", now - 500, "speaking"});

    // 清理60秒以上的记录
    cache.Cleanup(60000);

    // 应该只剩2条
    size_t size = cache.Size();
    if (size != 2) {
        ESP_LOGE(TAG, "Test 4.1 FAILED: expected size 2 after cleanup, got %u", size);
        return false;
    }
    ESP_LOGI(TAG, "Test 4.1 PASSED: Expired records cleaned up");

    // 验证旧记录已删除
    std::string active_id = cache.GetLatestId("active");
    if (active_id == "img_old_001") {
        ESP_LOGE(TAG, "Test 4.2 FAILED: Old record should be removed");
        return false;
    }
    ESP_LOGI(TAG, "Test 4.2 PASSED: Old record removed");

    ESP_LOGI(TAG, "=== Test 4: PASSED ===\n");
    return true;
}

// 测试5：线程安全测试
bool TestThreadSafety() {
    ESP_LOGI(TAG, "=== Test 5: Thread Safety ===");

    ImageIdCache cache(100);

    // 启动多个线程并发添加记录
    const int NUM_THREADS = 4;
    const int RECORDS_PER_THREAD = 25;

    auto add_records = [&](int thread_id) {
        for (int i = 0; i < RECORDS_PER_THREAD; i++) {
            std::string id = "img_thread_" + std::to_string(thread_id) + "_" + std::to_string(i);
            cache.Add(ImageIdRecord{id, esp_timer_get_time() / 1000, "test"});
            // 小延迟增加并发冲突概率
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    // 创建线程
    std::thread threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        threads[i] = std::thread(add_records, i);
    }

    // 等待所有线程完成
    for (int i = 0; i < NUM_THREADS; i++) {
        threads[i].join();
    }

    // 验证总记录数
    size_t size = cache.Size();
    if (size != NUM_THREADS * RECORDS_PER_THREAD) {
        ESP_LOGE(TAG, "Test 5.1 FAILED: expected %u records, got %u",
                NUM_THREADS * RECORDS_PER_THREAD, size);
        return false;
    }
    ESP_LOGI(TAG, "Test 5.1 PASSED: All %u records added concurrently", size);

    // 清空测试
    cache.Clear();
    size = cache.Size();
    if (size != 0) {
        ESP_LOGE(TAG, "Test 5.2 FAILED: expected size 0 after clear, got %u", size);
        return false;
    }
    ESP_LOGI(TAG, "Test 5.2 PASSED: Cache cleared successfully");

    ESP_LOGI(TAG, "=== Test 5: PASSED ===\n");
    return true;
}

// 运行所有测试
void RunAllTests() {
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "  ImageIdCache Unit Tests");
    ESP_LOGI(TAG, "========================================\n");

    int passed = 0;
    int total = 5;

    if (TestBasicOperations()) passed++;
    if (TestSizeLimit()) passed++;
    if (TestContextFilter()) passed++;
    if (TestExpiration()) passed++;
    if (TestThreadSafety()) passed++;

    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "  Test Results: %d/%d PASSED", passed, total);
    ESP_LOGI(TAG, "========================================\n");

    if (passed == total) {
        ESP_LOGI(TAG, "✅ All tests PASSED!");
    } else {
        ESP_LOGE(TAG, "❌ Some tests FAILED!");
    }
}

}  // namespace TestImageIdCache
