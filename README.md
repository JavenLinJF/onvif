# demo虚拟机运行步骤

## 安装

```cmake
sudo apt update
sudo apt install -y \
    libcurl4-openssl-dev \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libavfilter-dev \
    pkg-config \
    build-essential \
    cmake
```

## 编译

```cd /home/javen/example/onvif
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

## 运行
```./onvif_demo
```

## 输出结果

```
[onvif] C API 演示程序（独立 demo）：
[onvif]   init → discover → 主/子码流拉流 → 主/子抓拍 → 5s 后停止
[onvif] ---------- 设备信息（ONVIF 探测完成）----------
[onvif] device_service: http://192.168.110.21:8000/onvif/device_service
[onvif] 认证用户: admin（密码已省略）
[onvif] 厂商=Happytimesoft 型号=IPCamera 固件=2.4
[onvif] 序列号=123456 硬件ID=1.0 主机名=DESKTOP-FLU3P9M
[onvif] Media XAddr=http://192.168.110.21:8000/onvif/media_service
[onvif] PTZ XAddr=http://192.168.110.21:8000/onvif/ptz_service
[onvif] 主 ProfileToken=ProfileToken_1 视频编码=H264
[onvif] 主码流 RTSP: rtsp://192.168.110.21/test.mp4&amp;t=unicast&amp;p=rtsp&amp;ve=H264&amp;w=1280&amp;h=720&amp;ae=PCMU&amp;sr=8000
[onvif] 子码流 RTSP: rtsp://192.168.110.21/test.mp4&amp;t=unicast&amp;p=rtsp&amp;ve=H264&amp;w=640&amp;h=480&amp;ae=PCMU&amp;sr=8000
[onvif] 子码流 视频编码=H264
[onvif] 主抓拍 URL: http://192.168.110.21:8000/snapshot/ProfileToken_1
[onvif] 子抓拍 URL: http://192.168.110.21:8000/snapshot/ProfileToken_2
[onvif] --------------------------------------------
[onvif] 发现 1 台设备（按 IP 升序；每台详情已在探测时打印）
[onvif] 设备 #0 主码流 拉流已启动: rtsp://192.168.110.21/test.mp4&amp;t=unicast&amp;p=rtsp&amp;ve=H264&amp;w=1280&amp;h=720&amp;ae=PCMU&amp;sr=8000
[onvif] 设备 #0 子码流 拉流已启动: rtsp://192.168.110.21/test.mp4&amp;t=unicast&amp;p=rtsp&amp;ve=H264&amp;w=640&amp;h=480&amp;ae=PCMU&amp;sr=8000
[onvif] 设备 #0 主码流 抓拍: http://192.168.110.21:8000/snapshot/ProfileToken_1
[onvif] 设备 #0 主码流 抓拍完成, JPEG 字节数=3453
[onvif] [Demo] 主码流抓拍 成功，JPEG 3453 字节 (设备 #0)
[onvif] 设备 #0 子码流 抓拍: http://192.168.110.21:8000/snapshot/ProfileToken_2
[onvif] 设备 #0 子码流 抓拍完成, JPEG 字节数=3453
[onvif] [Demo] 子码流抓拍 成功，JPEG 3453 字节 (设备 #0)
[onvif] [Demo] 拉流进行中，5 秒后停止…
[onvif] [主码流] 帧 #1 size=58590 codec=0 dev=0
[onvif] [主码流] 帧 #2 size=1024 codec=3 dev=0
[onvif] [主码流] 帧 #3 size=953 codec=0 dev=0
[onvif] [主码流] 帧 #4 size=29859 codec=0 dev=0
[onvif] [主码流] 帧 #5 size=4478 codec=0 dev=0
[onvif] [主码流] 帧 #6 size=6899 codec=0 dev=0
[onvif] [主码流] 帧 #7 size=1024 codec=3 dev=0
[onvif] [主码流] 帧 #8 size=10596 codec=0 dev=0
[onvif] [主码流] 帧 #9 size=9319 codec=0 dev=0
[onvif] [主码流] 帧 #10 size=15949 codec=0 dev=0
[onvif] [主码流] 帧 #11 size=1024 codec=3 dev=0
[onvif] [主码流] 帧 #12 size=9799 codec=0 dev=0
[onvif] [主码流] 帧 #13 size=19727 codec=0 dev=0
[onvif] [主码流] 帧 #14 size=8110 codec=0 dev=0
[onvif] [主码流] 帧 #15 size=8296 codec=0 dev=0
[onvif] [主码流] 帧 #16 size=1024 codec=3 dev=0
[onvif] [主码流] 帧 #17 size=8950 codec=0 dev=0
[onvif] [主码流] 帧 #18 size=13821 codec=0 dev=0
[onvif] [主码流] 帧 #19 size=1024 codec=3 dev=0
[onvif] [主码流] 帧 #20 size=9951 codec=0 dev=0
[onvif] [子码流] 帧 #1 size=8535 codec=0 dev=0
[onvif] [子码流] 帧 #2 size=26 codec=0 dev=0
[onvif] [子码流] 帧 #3 size=1024 codec=3 dev=0
[onvif] [子码流] 帧 #4 size=653 codec=0 dev=0
[onvif] [子码流] 帧 #5 size=940 codec=0 dev=0
[onvif] [子码流] 帧 #6 size=1286 codec=0 dev=0
[onvif] [子码流] 帧 #7 size=1024 codec=3 dev=0
[onvif] [子码流] 帧 #8 size=2496 codec=0 dev=0
[onvif] [子码流] 帧 #9 size=1276 codec=0 dev=0
[onvif] [子码流] 帧 #10 size=1239 codec=0 dev=0
[onvif] [子码流] 帧 #11 size=1024 codec=3 dev=0
[onvif] [子码流] 帧 #12 size=1642 codec=0 dev=0
[onvif] [子码流] 帧 #13 size=1452 codec=0 dev=0
[onvif] [子码流] 帧 #14 size=2036 codec=0 dev=0
[onvif] [子码流] 帧 #15 size=1680 codec=0 dev=0
[onvif] [子码流] 帧 #16 size=1024 codec=3 dev=0
[onvif] [子码流] 帧 #17 size=1679 codec=0 dev=0
[onvif] [子码流] 帧 #18 size=1688 codec=0 dev=0
[onvif] [子码流] 帧 #19 size=1708 codec=0 dev=0
[onvif] [子码流] 帧 #20 size=1024 codec=3 dev=0
[onvif] [Demo] 结束

```
