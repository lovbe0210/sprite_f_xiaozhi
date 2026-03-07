#!/usr/bin/env python3
"""
ESP32摄像头服务 - 服务端示例代码

这是服务端接收图片上传的示例实现，使用Python Flask框架。
可以根据实际使用的框架（Express/FastAPI等）进行调整。

功能：
1. 接收ESP32上传的JPEG图片
2. 生成唯一的image_id
3. 可选：执行人脸/手势检测
4. 返回image_id给客户端
5. 提供图片访问接口
"""

from flask import Flask, request, jsonify, send_file
from werkzeug.utils import secure_filename
import os
import uuid
import time
from datetime import datetime
from pathlib import Path

# ==================== 配置 ====================
UPLOAD_FOLDER = './uploads'
ALLOWED_EXTENSIONS = {'jpg', 'jpeg'}
MAX_FILE_SIZE = 10 * 1024 * 1024  # 10MB

app = Flask(__name__)
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER
app.config['MAX_CONTENT_LENGTH'] = MAX_FILE_SIZE

# 确保上传目录存在
Path(UPLOAD_FOLDER).mkdir(parents=True, exist_ok=True)

# ==================== 辅助函数 ====================
def allowed_file(filename):
    return '.' in filename and \
           filename.rsplit('.', 1)[1].lower() in ALLOWED_EXTENSIONS


def generate_image_id(context):
    """生成唯一的image_id"""
    timestamp = int(time.time() * 1000)
    unique_id = str(uuid.uuid4())[:8]
    return f"img_{timestamp}_{context}_{unique_id}"


def detect_faces(image_path):
    """
    可选：人脸检测（需要安装opencv-python）
    pip install opencv-python
    """
    try:
        import cv2
        import numpy as np

        # 加载图片
        img = cv2.imread(image_path)
        if img is None:
            return []

        # 转换为灰度图
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

        # 加载Haar级联分类器
        face_cascade = cv2.CascadeClassifier(
            cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'
        )

        # 检测人脸
        faces = face_cascade.detectMultiScale(
            gray,
            scaleFactor=1.1,
            minNeighbors=5,
            minSize=(30, 30)
        )

        results = []
        for (x, y, w, h) in faces:
            results.append({
                'bbox': [int(x), int(y), int(w), int(h)],
                'confidence': 0.95  # 这里简化处理，实际可以使用更准确的模型
            })

        return results
    except ImportError:
        print("OpenCV not installed, skipping face detection")
        return []
    except Exception as e:
        print(f"Face detection error: {e}")
        return []


# ==================== HTTP接口 ====================

@app.route('/api/camera/upload', methods=['POST'])
def upload_image():
    """
    接收ESP32上传的图片

    请求格式：multipart/form-data
    - context: "active", "listening", "speaking"
    - timestamp: 毫秒时间戳
    - file: JPEG图片文件

    返回格式：
    {
        "success": true,
        "image_id": "img_xxx",
        "url": "https://...",
        "size": 12345,
        "detection_result": {
            "faces": [...],
            "gestures": [],
            "should_wake": false
        }
    }
    """
    try:
        # 检查文件
        if 'file' not in request.files:
            return jsonify({
                'success': False,
                'message': 'No file part'
            }), 400

        file = request.files['file']
        if file.filename == '':
            return jsonify({
                'success': False,
                'message': 'No selected file'
            }), 400

        if not allowed_file(file.filename):
            return jsonify({
                'success': False,
                'message': 'Invalid file type'
            }), 400

        # 获取表单字段
        context = request.form.get('context', 'unknown')
        timestamp = request.form.get('timestamp', str(int(time.time() * 1000)))
        device_id = request.headers.get('Device-Id', 'unknown')
        client_id = request.headers.get('Client-Id', 'unknown')

        # 生成image_id
        image_id = generate_image_id(context)

        # 保存文件
        file_extension = 'jpg'
        filename = f"{image_id}.{file_extension}"
        filepath = os.path.join(app.config['UPLOAD_FOLDER'], filename)
        file.save(filepath)

        # 获取文件大小
        file_size = os.path.getsize(filepath)

        # 可选：执行检测
        faces = detect_faces(filepath)

        # 判断是否应该唤醒设备（检测到人脸）
        should_wake = len(faces) > 0 and context == 'active'

        # 记录日志
        print(f"[{datetime.now()}] Image uploaded:")
        print(f"  Image ID: {image_id}")
        print(f"  Context: {context}")
        print(f"  Device ID: {device_id}")
        print(f"  Client ID: {client_id}")
        print(f"  File size: {file_size} bytes")
        print(f"  Faces detected: {len(faces)}")
        print(f"  Should wake: {should_wake}")

        # 构造响应
        response = {
            'success': True,
            'image_id': image_id,
            'size': file_size,
            'url': f"http://{request.host}/api/camera/image/{image_id}",
            'detection_result': {
                'faces': faces,
                'gestures': [],
                'should_wake': should_wake
            }
        }

        return jsonify(response), 200

    except Exception as e:
        print(f"Error uploading image: {e}")
        return jsonify({
            'success': False,
            'message': str(e)
        }), 500


@app.route('/api/camera/image/<image_id>', methods=['GET'])
def get_image(image_id):
    """
    根据image_id获取图片

    参数：
    - image_id: 图片ID（不含扩展名）

    返回：JPEG图片文件
    """
    try:
        # 查找文件
        filename = f"{image_id}.jpg"
        filepath = os.path.join(app.config['UPLOAD_FOLDER'], filename)

        if not os.path.exists(filepath):
            return jsonify({
                'success': False,
                'message': 'Image not found'
            }), 404

        return send_file(filepath, mimetype='image/jpeg')

    except Exception as e:
        return jsonify({
            'success': False,
            'message': str(e)
        }), 500


@app.route('/api/camera/list', methods=['GET'])
def list_images():
    """
    列出所有已上传的图片

    返回：
    {
        "success": true,
        "images": [
            {
                "image_id": "img_xxx",
                "size": 12345,
                "timestamp": "...",
                "context": "active"
            },
            ...
        ]
    }
    """
    try:
        images = []
        upload_path = Path(app.config['UPLOAD_FOLDER'])

        for filepath in upload_path.glob('img_*.jpg'):
            image_id = filepath.stem
            file_size = filepath.stat().st_size
            mtime = datetime.fromtimestamp(filepath.stat().st_mtime)

            images.append({
                'image_id': image_id,
                'size': file_size,
                'timestamp': mtime.isoformat(),
                'url': f"http://{request.host}/api/camera/image/{image_id}"
            })

        # 按修改时间倒序排列
        images.sort(key=lambda x: x['timestamp'], reverse=True)

        return jsonify({
            'success': True,
            'images': images
        })

    except Exception as e:
        return jsonify({
            'success': False,
            'message': str(e)
        }), 500


@app.route('/api/health', methods=['GET'])
def health_check():
    """健康检查接口"""
    return jsonify({
        'status': 'ok',
        'service': 'xiaozhi-camera-service',
        'version': '1.0.0'
    })


# ==================== 主程序 ====================
if __name__ == '__main__':
    print("========================================")
    print("  Xiaozhi Camera Service")
    print("========================================")
    print(f"Upload folder: {UPLOAD_FOLDER}")
    print(f"Max file size: {MAX_FILE_SIZE / 1024 / 1024} MB")
    print("Available endpoints:")
    print("  POST /api/camera/upload")
    print("  GET  /api/camera/image/<image_id>")
    print("  GET  /api/camera/list")
    print("  GET  /api/health")
    print("========================================")

    # 启动服务器
    app.run(host='0.0.0.0', port=5000, debug=True)
