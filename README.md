# 仓储机器人上位机可视化系统

基于 **Qt 6 + C++17** 开发的工业仓储自动化上位机软件，运行于 Windows / Linux 工控机，通过 Modbus TCP 控制华沿机械臂和仙工 AGV，通过 HTTP 与视觉服务通信，实现抓取→运输→卸料的完整自动化循环。

---

## 硬件配置

| 设备 | 型号 | 通信协议 | 默认 IP |
|------|------|----------|---------|
| 协作机械臂 | 华沿 E05 / E10 / S60 | Modbus TCP | 192.168.10.10:502 |
| 移动机器人 | 仙工 AGV | Modbus TCP | 192.168.192.5:502 |
| 深度相机 | 奥比中光 Femto / Astra | HTTP (Flask) | 192.168.1.10:8080 |
| 扫码器 | NLS-NVF230V2 | TCP | 192.168.2.100 |
| 补光灯 | GPIO 控制 (PCF9554) | Linux GPIO | 仅 Linux |

---

## 软件依赖

| 环境 | 要求 |
|------|------|
| Qt | 6.8+ |
| Qt 模块 | `Widgets` `Network` `SerialBus` |
| C++ | C++17 |
| CMake | 3.16+ |
| 编译器（开发）| MinGW 64-bit 或 MSVC 2022（Windows） |
| 编译器（部署）| GCC 9+（Linux） |
| Python（相机）| 3.8+，pyorbbecsdk，Flask，ultralytics，sam2 |

---

## 项目结构

```
wh-robot-visual/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── mainwindow.{h,cpp}        # 纯 UI 层：布局 + 信号转发
│   ├── workflowengine.{h,cpp}    # 业务核心：五步状态机
│   ├── devicemanager.{h,cpp}     # 设备生命周期管理
│   ├── robotcontroller.{h,cpp}   # 机械臂 Modbus TCP
│   ├── agvcontroller.{h,cpp}     # AGV Modbus TCP
│   ├── visionclient.{h,cpp}      # 视觉 HTTP + 坐标转换
│   ├── camerawindow.{h,cpp}      # 相机实时预览对话框
│   ├── handeyedialog.{h,cpp}     # 手眼矩阵加载向导
│   ├── deviceindicator.{h,cpp}   # LED 状态指示灯控件
│   ├── workflowwidget.{h,cpp}    # 流程图可视化控件
│   └── themeswitch.{h,cpp}       # 深色/浅色主题切换
├── scripts/
│   ├── camera_stream.py          # 相机画面流媒体脚本
│   └── camera_test.py
├── tools/
│   └── fill_light                # 补光灯 GPIO 控制程序（Linux）
├── Log/                          # 运行日志（自动创建，格式：yyyy-MM-dd log.txt）
└── changelog/
    └── CHANGELOG.md
```

**视觉服务**（独立仓库 / 子目录）：

```
View_data_http/
└── main_linux_auto.py   # Flask HTTP 服务：YOLO + SAM2 + Orbbec RGB-D
hand-eye/
└── calibration_output/
    └── handeye_matrix.txt   # 手眼标定矩阵（4×4，行主序浮点数）
```

---

## 快速开始

### 1. 克隆仓库

```bash
git clone https://github.com/ssrxiaoge2/robot_visual.git
cd robot_visual/wh-robot-visual
```

### 2. 编译（Windows 开发）

用 Qt Creator 打开 `CMakeLists.txt`，选择 Qt 6.8+ MinGW/MSVC kit，直接 Build。

> Linux 专属功能（Orbbec SDK、GPIO 补光灯）在 Windows 编译时自动跳过，不影响 UI 和仿真调试。

### 3. 编译（Linux 部署）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 4. 运行视觉服务（Linux 工控机）

```bash
cd View_data_http
python3 main_linux_auto.py   # Flask 在 8080 端口监听
```

### 5. 使用上位机

1. 填写各设备 IP，点击"应用配置"建立 Modbus 连接
2. 使用各设备旁"测试"按钮验证 TCP 可达性
3. 点击"▶ 开始运行"启动自动循环；无硬件时自动进入仿真模式
4. "▶| 单步运行"可手动逐步推进，便于调试

---

## 五步工作流时序

正确的硬件时序（与机械臂控制器约定）：

```
Step 0-A  写 CmdID=8 → 机器人运动到拍照等待位
Step 0-B  轮询 Input1132==1（机器人到位请求拍照）→ 触发视觉推理
          视觉服务返回 (offset_mm, depth, angle_deg)
          坐标转换：相机系 → 手眼矩阵 → 机器人工具系 → Holding 901-906 寄存器值

Step 1    写坐标到 Holding 901-906
          轮询 Input1132==2（机器人抓取完成）

Step 2    写目标工位到 AGV Holding 1000
          轮询 AGV Input1001==2（AGV 到位）
          故障(3)：停止流程，写 CmdID=0 复位机械臂

Step 3    写 CmdID=9（翻转卸料）
          轮询 Input1132==3（卸料完成）

Step 4    写 CmdID=0（复位）
          轮询 Input1132==0（空闲就绪）
          工位 1→2→…→12→1 循环，进入下一轮 Step 0
```

**仿真模式**：任何设备未连接时，对应步骤自动按步进延时推进，无需硬件即可演示完整流程。

---

## Modbus 寄存器速查

### 华沿机械臂（192.168.10.10:502）

| 地址 | 类型 | 说明 |
|------|------|------|
| Holding 900 | 写 | CmdID：8=抓取程序，9=翻转卸料，0=复位 |
| Holding 901–906 | 写 | X / Y / Z / Rx / Ry / Rz（单位 0.001 mm / 0.001°） |
| Input 1132 | 读 | 0=空闲，1=请求拍照，2=抓取完成，3=卸料完成 |

### 仙工 AGV（192.168.192.5:502）

| 地址 | 类型 | 说明 |
|------|------|------|
| Holding 1000 | 写 | 目标工位编号（1–12） |
| Input 1001 | 读 | 0=空闲，1=行走中，2=已到位，3=故障 |

> 以上地址为当前约定值，部署前务必对照设备接口手册验证。

---

## 当前功能状态

| 功能 | 状态 | 说明 |
|------|------|------|
| UI 布局 + 深浅色主题 | 完成 | 2×2 设备状态网格，动画主题切换 |
| 仿真模式五步循环 | 完成 | 无硬件可完整演示 |
| 机械臂 Modbus TCP | 完成 | FC03/04/06/16，含调试面板 |
| AGV Modbus TCP | 完成 | 工位调度 + 到位轮询 + 故障停机 |
| AGV 故障自动停机 | 完成 | 故障时停流程并写 CmdID=0 复位机械臂 |
| 视觉 HTTP 客户端 | 完成 | /inference 调用 + 手眼矩阵坐标转换 |
| 相机实时预览 | 完成 | /frame/annotated JPEG 流 |
| 手眼矩阵加载向导 | 完成 | 三步向导，支持文件加载和手动填写 |
| 补光灯控制 | 完成 | Linux GPIO，Windows 下静默跳过 |
| 日志持久化 | 完成 | 运行日志自动保存到 Log/<日期> log.txt |
| 扫码器数据读取 | 未实现 | 仅 TCP 连通性测试，无数据协议 |
| MES 接口 | 未实现 | API 文档待提供 |
| 联机集成测试 | 未完成 | 待硬件在线验证 |

---

## 下一步工作

### 第一优先级：联机调试（需要硬件）

以下内容必须连接真实设备才能完成：

**1. 确认 Modbus 寄存器映射**
- 连接机械臂，读取 Input 1132 验证状态字实际值（0/1/2/3 是否与协议一致）
- 确认 Holding 900 命令字：CmdID=8 是否触发抓取程序，CmdID=9 是否触发翻转卸料
- 连接 AGV，验证 Holding 1000 写入工位编号后 AGV 是否响应

**2. 设置 baseRzReg 基准值**

文件：`src/visionclient.cpp`，找到 `m_baseRzReg` 成员变量（当前默认值 0）：
```cpp
// 联机后读取机器人在拍照位时 Holding 906 的实际值，填入此处
int m_baseRzReg = 0;  // TODO: 需联机调试后设置实际值
```

**3. 重新执行手眼标定**

当前矩阵误差约 2–3 mm，精度不足时需重新标定：
```bash
# 参考 hand-eye/ 目录下的标定流程
# 标定完成后将新矩阵写入：
# hand-eye/calibration_output/handeye_matrix.txt
# 再通过上位机"手眼标定矩阵"按钮加载并应用
```

**4. 完整流程集成测试**
- 仿真模式验证通过后，打开机械臂和 AGV 的 Modbus 连接
- 单步模式逐步走通五步流程，观察寄存器读写和状态字变化
- 自动循环模式下连续运行，验证工位轮转和循环计数正确性

---

### 第二优先级：Python 视觉服务修复（可立即做）

文件：`View_data_http/main_linux_auto.py`

当前问题：文件内有硬编码的 Windows 路径（`D:\View_data\...`），在 Linux 工控机上无法运行。

需要修复为相对路径或通过命令行参数传入：
```python
# 需要检查并修改所有类似以下的硬编码路径：
# model_path = "D:\\View_data\\models\\yolo.pt"  # 错误
# model_path = os.path.join(os.path.dirname(__file__), "models", "yolo.pt")  # 正确
```

---

### 第三优先级：扫码器集成（需要协议文档）

设备：NLS-NVF230V2，通过 TCP 通信。

当前状态：仅实现了 TCP 连通性测试，无数据读取逻辑。

**需要提供**：NLS-NVF230V2 TCP 通信协议手册（触发指令、数据格式、帧结构）。

实现目标：
- 在 Step 1（机械臂夹爪到达抓取点后）并发触发扫码
- 扫码成功 → AGV 前往正常工位
- 扫码失败 → AGV 前往异常暂存区（当前占位符工位编号待确认）

---

### 第四优先级：MES 接口对接（需要 API 文档）

当前状态：无任何 MES 相关代码。

**需要提供**：MES REST API 文档（鉴权方式、上传字段、接口地址）。

实现目标：
- 扫码获取料箱编号后，上传上料记录到 MES
- 异常料箱上传 MES 并标记异常状态

---

### 其他待确认事项

| 事项 | 说明 |
|------|------|
| AGV 异常工位编号 | 当前代码占位符为 0，需与现场确认实际工位编号 |
| CMD_FLIP 实际值 | 翻转卸料命令字当前为 9，需对照华沿机器人操作手册确认 |
| AGV 工位数量 | 当前代码支持 1–12 工位循环，需与现场工位数对齐 |
| 扫码时机 | 夹爪到达抓取点后才扫码，扫码等待时间需与机械臂动作时序协调 |

---

## 架构说明

```
MainWindow（UI 层）
    ├── WorkflowEngine      五步状态机，不含任何 UI 代码
    └── DeviceManager       设备生命周期管理
            ├── RobotController   华沿机械臂 Modbus TCP
            ├── AgvController     仙工 AGV Modbus TCP
            └── VisionHttpClient  视觉 HTTP + 手眼坐标变换
```

核心原则：
- **MainWindow** 只做布局和信号转发，不含 Modbus / 网络逻辑
- **WorkflowEngine** 不依赖任何 Widget，通过信号与 UI 通信
- **双模式运行**：同一代码，有硬件走 Modbus，无硬件自动降级仿真

---

## 开发规范

详见 `CLAUDE.md`，要点：

- 分支策略：`feature/<名称>` 开发，PR 合并到 `main`，不直接 push main
- Commit 格式：`feat:` / `fix:` / `refactor:` / `docs:` 前缀
- 每次 PR 前更新 `changelog/CHANGELOG.md`
- 新增有颜色控件必须在 `applyTheme(bool dark)` 中同步适配深浅色

---

## License

内部项目，暂不开源。
