#!/bin/bash
#
# ESP32摄像头上传测试脚本
#
# 用于测试HTTP接口是否正常工作
#

SERVER_URL="http://localhost:5000"
TEST_IMAGE="${1:-./test_image.jpg}"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================"
echo "  ESP32 Camera Upload Test"
echo "========================================"
echo "Server URL: $SERVER_URL"
echo "Test Image: $TEST_IMAGE"
echo ""

# 检查test_image是否存在
if [ ! -f "$TEST_IMAGE" ]; then
    echo -e "${RED}Error: Test image not found: $TEST_IMAGE${NC}"
    echo ""
    echo "Please provide a test image:"
    echo "  $0 /path/to/test_image.jpg"
    echo ""
    echo "Or create a simple test image:"
    echo "  # Create a 640x480 red JPEG image (requires ImageMagick)"
    echo "  convert -size 640x480 xc:red test_image.jpg"
    exit 1
fi

# 1. 健康检查
echo -n "[1/4] Health check... "
HEALTH_RESPONSE=$(curl -s "$SERVER_URL/api/health")
if echo "$HEALTH_RESPONSE" | grep -q '"status": "ok"'; then
    echo -e "${GREEN}OK${NC}"
    echo "       $HEALTH_RESPONSE"
else
    echo -e "${RED}FAILED${NC}"
    echo "       $HEALTH_RESPONSE"
    exit 1
fi
echo ""

# 2. 上传图片（主动模式）
echo -n "[2/4] Uploading image (active mode)... "
UPLOAD_RESPONSE=$(curl -s -X POST "$SERVER_URL/api/camera/upload" \
  -H "Device-Id: ESP32_TEST_001" \
  -H "Client-Id: test-client-123" \
  -F "context=active" \
  -F "timestamp=$(date +%s%3N)" \
  -F "file=@$TEST_IMAGE")

if echo "$UPLOAD_RESPONSE" | grep -q '"success": true'; then
    echo -e "${GREEN}OK${NC}"

    # 提取image_id
    IMAGE_ID=$(echo "$UPLOAD_RESPONSE" | grep -o '"image_id":"[^"]*"' | cut -d'"' -f4)
    echo "       Image ID: $IMAGE_ID"

    # 提取url
    IMAGE_URL=$(echo "$UPLOAD_RESPONSE" | grep -o '"url":"[^"]*"' | cut -d'"' -f4)
    echo "       URL: $IMAGE_URL"

    # 检测结果
    if echo "$UPLOAD_RESPONSE" | grep -q '"should_wake":true'; then
        echo -e "       ${YELLOW}Should wake: YES${NC}"
    else
        echo "       Should wake: NO"
    fi
else
    echo -e "${RED}FAILED${NC}"
    echo "       $UPLOAD_RESPONSE"
    exit 1
fi
echo ""

# 3. 获取图片
echo -n "[3/4] Retrieving image... "
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$SERVER_URL/api/camera/image/$IMAGE_ID")
if [ "$HTTP_CODE" = "200" ]; then
    echo -e "${GREEN}OK${NC}"
    echo "       HTTP $HTTP_CODE"
else
    echo -e "${RED}FAILED${NC}"
    echo "       HTTP $HTTP_CODE"
fi
echo ""

# 4. 列出所有图片
echo -n "[4/4] Listing images... "
LIST_RESPONSE=$(curl -s "$SERVER_URL/api/camera/list")
if echo "$LIST_RESPONSE" | grep -q '"success": true'; then
    echo -e "${GREEN}OK${NC}"

    # 计数
    COUNT=$(echo "$LIST_RESPONSE" | grep -o '"image_id"' | wc -l)
    echo "       Total images: $COUNT"
else
    echo -e "${RED}FAILED${NC}"
    echo "       $LIST_RESPONSE"
fi
echo ""

# 测试不同context
echo "========================================"
echo "  Testing Different Contexts"
echo "========================================"

CONTEXTS=("listening" "speaking" "active")
for CONTEXT in "${CONTEXTS[@]}"; do
    echo -n "Testing context: $CONTEXT ... "

    RESPONSE=$(curl -s -X POST "$SERVER_URL/api/camera/upload" \
      -H "Device-Id: ESP32_TEST_001" \
      -F "context=$CONTEXT" \
      -F "timestamp=$(date +%s%3N)" \
      -F "file=@$TEST_IMAGE")

    if echo "$RESPONSE" | grep -q '"success": true'; then
        ID=$(echo "$RESPONSE" | grep -o '"image_id":"[^"]*"' | cut -d'"' -f4)
        echo -e "${GREEN}OK${NC} ($ID)"
    else
        echo -e "${RED}FAILED${NC}"
    fi
done
echo ""

echo "========================================"
echo -e "${GREEN}All tests completed!${NC}"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Update ESP32 config:"
echo "     CONFIG_CAMERA_UPLOAD_URL=\"$SERVER_URL/api/camera/upload\""
echo "     CONFIG_CAMERA_UPLOAD_TOKEN=\"\""
echo ""
echo "  2. Flash and test on device"
echo ""
echo "  3. Monitor uploaded images:"
echo "     $0  # List all images"
