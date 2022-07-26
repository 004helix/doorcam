Doorcam video processing:
- read mjpeg stream from /dev/video0: 1920x1080 @ 30fps
- stream0 1920x1080 30fps @ http://127.0.0.1:8080
- stream1 960x540 5 fps @ http://127.0.0.1:8081
- record video when motion detected
- live qrcode scanner (vaapi -> opencl -> opencv undistord -> wechat dnn detector -> dymansoft barcode reader)
