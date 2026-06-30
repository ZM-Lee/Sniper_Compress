# RMCC Sniper 相机压缩发送端

本项目是 RMCC 低码率相机画面发送端。当前链路使用大恒 Daheng Galaxy 相机采集图像，通过 C++ 压缩模块编码后，可以选择通过串口 0x0310 或 MQTT 发送出去。

主要数据流：

```text
Daheng camera -> shared memory -> compress -> serial 0x0310
Daheng camera -> shared memory -> compress -> MQTT CustomByteBlock
```

旧 `Sniper/` 项目只作为迁移来源存在，新方案需要的头文件、protobuf 和 MQTT 发送代码已经放进当前项目目录。后续删除 `Sniper/` 文件夹不应影响当前项目运行。

## 目录说明

```text
include/                     公共头文件
include/compress/            压缩、打包相关头文件
include/daheng_camera/       大恒相机头文件和封装
include/mqtt/                MQTT 发送接口
include/protobuf/            rmcc.proto 与生成的 protobuf 头文件
src/                         C++ 源码
src/camera/                  大恒相机采集程序
src/cli/                     compress 命令入口
src/compress/                压缩、码流打包、共享内存等核心代码
src/mqtt/                    MQTT 发送与 protobuf cpp
src/serial/                  串口 0x0310 发送
config/daheng_camera/        大恒相机配置
config/mqtt/                 MQTT 默认配置
config/sender.env            发送端默认参数
scripts/                     构建、运行、检查脚本
models/                      OpenVINO/TensorRT 模型
bin/                         build 后复制出的可运行文件
build/                       CMake 构建目录
```

## 生成文件

编译后主要生成三个文件：

```text
bin/librmcc_sniper.so        压缩核心动态库
bin/camera_capture           大恒相机采集程序，写入 shared memory
bin/compress                 压缩并通过 serial/MQTT 发送的程序
```

`scripts/run_camera_sender.sh` 默认调用 `bin/` 下的程序。因此每次修改 C++ 或 CMake 后，都要重新运行：

```bash
./scripts/build_sender.sh
```

否则可能出现参数不存在、旧符号找不到等问题。

## 依赖

核心依赖：

- CMake >= 3.16
- C++17 编译器
- OpenVINO Runtime
- Daheng Galaxy SDK，提供 `GxIAPI.h` 和 `libgxiapi`
- Protobuf
- libmosquitto

Ubuntu 上常见依赖包：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake \
  libprotobuf-dev protobuf-compiler \
  libmosquitto-dev mosquitto mosquitto-clients
```

OpenVINO 和大恒 Galaxy SDK 请按对应平台安装。安装完成后可以检查：

```bash
./scripts/check_env.sh
```

Intel NUC 上建议锁频，减少推理抖动：

```bash
sudo ./scripts/optimize_gpu_freq.sh
```

串口发送还需要用户有串口权限：

```bash
sudo usermod -aG dialout "$USER"
```

执行后退出登录再重新登录。

## 编译

普通 OpenVINO 版本：

```bash
./scripts/build_sender.sh
```

如果大恒 SDK 不在系统默认路径，可以指定：

```bash
DAHENG_ROOT=/path/to/Galaxy_camera ./scripts/build_sender.sh
```

如果要启用 TensorRT g_a 后端：

```bash
ENABLE_TRT=1 ./scripts/build_sender.sh
```

也可以手动使用 CMake：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

但推荐使用 `scripts/build_sender.sh`，因为它会自动把产物复制到 `bin/`。

## 默认配置

发送端默认参数在：

```text
config/sender.env
```

当前关键默认值：

```bash
SERIAL_PORT=auto
BAUDRATE=921600
MQTT_HOST=
MQTT_PORT=3333
MQTT_TOPIC=CustomByteBlock
MQTT_CLIENT_ID=doorlock_sniper
MQTT_QOS=1
CAMERA_CONFIG=config/daheng_camera/feature.yaml
CAMERA_INDEX=0
CAMERA_ROI_MODE=max-square
CAMERA_FPS=60
EXPOSURE_US=5000
TX_DEVICE=GPU.0
PRESET=qvrf192x2x24
PREBUFFER_CHUNKS=2
```

`MQTT_HOST` 为空时，`run_camera_sender.sh` 默认走串口。传入 `--mqtt-host` 后走 MQTT。

大恒相机参数在：

```text
config/daheng_camera/feature.yaml
```

MQTT 默认配置在：

```text
config/mqtt/server.yaml
```

## 快速自检

检查二进制、串口、OpenVINO 设备、模型文件：

```bash
./scripts/check_env.sh
```

不打开相机、不发送，只测试压缩：

```bash
./scripts/run_dry_sender.sh qvrf192x2x24
```

## 串口发送相机画面

最常用命令：

```bash
./scripts/run_camera_sender.sh \
  --preset qvrf192x2x24 \
  --prebuffer-chunks 4 \
  --serial-port /dev/ttyUSB0 \
  --baudrate 921600
```

让程序自动找串口：

```bash
./scripts/run_camera_sender.sh \
  --preset qvrf192x2x24 \
  --prebuffer-chunks 4 \
  --serial-port auto \
  --baudrate 921600
```

低延迟模式：

```bash
./scripts/run_camera_sender.sh \
  --preset qvrf192x1x24_lowlat \
  --prebuffer-chunks 2 \
  --serial-port /dev/ttyUSB0 \
  --baudrate 921600
```

高画质低帧率模式：

```bash
./scripts/run_camera_sender.sh \
  --preset qvrf448x6x8 \
  --prebuffer-chunks 4 \
  --serial-port /dev/ttyUSB0 \
  --baudrate 921600
```

串口链路：

```text
camera_capture -> /dev/shm/rm_camera_frames -> compress -> serial 0x0310
```

## MQTT 发送相机画面

先确保 MQTT broker 已运行。比如本机 broker：

```bash
sudo systemctl start mosquitto
```

发送到本机 MQTT：

```bash
./scripts/run_camera_sender.sh \
  --preset qvrf192x2x24 \
  --prebuffer-chunks 4 \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 3333 \
  --mqtt-topic CustomByteBlock
```

发送到其他机器：

```bash
./scripts/run_camera_sender.sh \
  --preset qvrf192x2x24 \
  --prebuffer-chunks 4 \
  --mqtt-host 192.168.12.1 \
  --mqtt-port 3333 \
  --mqtt-topic CustomByteBlock \
  --mqtt-client-id doorlock_sniper \
  --mqtt-qos 1
```

MQTT 链路：

```text
camera_capture -> /dev/shm/rm_camera_frames -> compress -> rmcc::CustomByteBlock -> MQTT
```

`compress` 会把每个 300B 画面 chunk 包进 `rmcc::CustomByteBlock` protobuf，然后发布到指定 topic。

## 常用 preset

```text
qvrf192x1x24_lowlat   192 输入，1 chunk/frame，24 FPS，低延迟
qvrf192x2x24          192 输入，2 chunks/frame，24 FPS，默认推荐
qvrf448x6x8           448 输入，6 chunks/frame，8 FPS，画质更高但帧率低
```

一般先用：

```bash
./scripts/run_camera_sender.sh --preset qvrf192x2x24
```

如果接收端偶尔卡顿或 `TX underruns` 较多，可以增加预缓冲：

```bash
./scripts/run_camera_sender.sh --preset qvrf192x2x24 --prebuffer-chunks 4
```

## 直接运行二进制

一般推荐脚本，但也可以手动拆开运行。

启动相机采集：

```bash
bin/camera_capture \
  --device-index 0 \
  --camera-config config/daheng_camera/feature.yaml \
  --roi-mode max-square \
  --fps 60 \
  --exposure-us 5000 \
  --shm-name /rm_camera_frames \
  --slots 4
```

另一个终端串口发送：

```bash
bin/compress \
  --shm-input \
  --shm-name /rm_camera_frames \
  --codec msssim_qvrf \
  --qvrf-cpp-sender \
  -d GPU.0 \
  --tx-ga-backend openvino \
  --fps 24 \
  --codec-size 192 \
  --chunks-per-frame 2 \
  --prebuffer-chunks 4 \
  --chunk-rate-hz 48 \
  --profile \
  -p /dev/ttyUSB0 \
  -b 921600 \
  --serial-wait
```

另一个终端 MQTT 发送：

```bash
bin/compress \
  --shm-input \
  --shm-name /rm_camera_frames \
  --codec msssim_qvrf \
  --qvrf-cpp-sender \
  -d GPU.0 \
  --tx-ga-backend openvino \
  --fps 24 \
  --codec-size 192 \
  --chunks-per-frame 2 \
  --prebuffer-chunks 4 \
  --chunk-rate-hz 48 \
  --profile \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 3333 \
  --mqtt-topic CustomByteBlock
```

## 输出结果怎么看

健康输出通常类似：

```text
Errors:       0
Queue drops:  0 chunks
TX underruns: 0 ticks
Over budget:  0
MQTT stats:   ... publishes
Serial stats: ... frames
```

重点关注：

- `Errors`：发送错误，应该为 0。
- `Queue drops`：发送队列丢包，应该为 0。
- `Over budget`：压缩码流超过预算，应该为 0。
- `TX underruns`：发送线程等不到 chunk 的次数，少量可以接受，比赛建议尽量压低。
- `Chunk intervals max`：目标 48 chunks/s 时，正常间隔约 20.83ms。偶尔 41ms 表示跳了一拍。
- `Stage p99 / Stage max`：真正压缩耗时。24 FPS 每帧预算约 41.7ms。

如果 `Compress: max` 很大，但 profile 里的 `Stage max total` 很小，通常说明不是模型本身慢，而是相机取帧、系统调度或 shared memory 等外部等待造成的一次主循环抖动。

## 脚本说明

```text
scripts/build_sender.sh              编译并复制产物到 bin/
scripts/check_env.sh                 检查二进制、串口、OpenVINO、模型等环境
scripts/run_camera_sender.sh         推荐入口，启动相机采集和压缩发送
scripts/run_dry_sender.sh            dry-run 压缩测试，不打开相机、不发送
scripts/launch_camera_stream.sh      完整 GUI 链路启动脚本，依赖 client/ 接收端
scripts/convert_onnx_to_openvino.sh  将 models/*.onnx 转成 OpenVINO xml/bin
scripts/optimize_gpu_freq.sh         锁 Intel iGPU 频率并设置 CPU performance
```

## TensorRT g_a 后端

默认使用 OpenVINO：

```bash
--tx-ga-backend openvino
```

如果已经有当前机器可用的 TensorRT engine，可以使用：

```bash
./scripts/run_camera_sender.sh \
  --preset qvrf448x6x8 \
  --tx-ga-backend tensorrt \
  --tx-trt-engine models/engines/msssim_g_a_448_fp32_fixed.engine \
  --tx-trt-device 0
```

TensorRT engine 通常和机器、TensorRT 版本、输入形状强相关。如果加载失败，先回退到 OpenVINO。

## 常见问题

### `Unknown option: --mqtt-host`

说明 `bin/compress` 还是旧版本。重新编译并复制到 `bin/`：

```bash
./scripts/build_sender.sh
```

### `symbol lookup error: pack_frame`

通常是旧 `bin/compress` 和新 `bin/librmcc_sniper.so` 不匹配。重新运行：

```bash
./scripts/build_sender.sh
```

### 找不到相机

检查大恒 SDK、相机连接和配置：

```bash
./scripts/check_env.sh
bin/camera_capture --camera-config config/daheng_camera/feature.yaml
```

### 串口打不开

检查设备和权限：

```bash
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
groups
```

必要时：

```bash
sudo usermod -aG dialout "$USER"
```

然后重新登录。

### MQTT 连接失败

检查 broker 是否运行、端口是否正确：

```bash
systemctl status mosquitto
mosquitto_sub -h 127.0.0.1 -p 3333 -t CustomByteBlock
```

如果 broker 使用默认 1883 端口，运行时也要改成 `--mqtt-port 1883`。

## 注意事项

- 修改 C++ 或 CMake 后一定要运行 `./scripts/build_sender.sh`。
- 同一时间不要让多个程序占用同一个串口或同一个相机。
- 串口 0x0310 每个物理 chunk 固定为 300B，代码已经处理好。
- `h_a/h_s` 当前要求使用 OpenVINO FP32 CPU 路径，不建议随意量化或切到其他后端。
- NUC 上建议比赛前执行一次 `sudo ./scripts/optimize_gpu_freq.sh`。
