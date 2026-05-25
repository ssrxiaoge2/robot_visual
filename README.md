# 仓储机器人可视化上位机

基于 **Qt 6 + C++17** 开发的工业机器人产线可视化监控系统，适配华沿机械臂 + 仙工 AGV + 奥比中光 3D 相机的复合机器人产线。

---

## 功能概览

| 模块 | 说明 |
|------|------|
| 工作流引擎 | 五步循环状态机，支持真实 Modbus 硬件驱动和仿真回退双模式 |
| 机械臂控制 | 华沿机器人 Modbus TCP 客户端，写坐标 + 发指令 + 轮询状态字 |
| AGV 控制 | 仙工 AGV Modbus TCP 客户端，工位调度 + 到位状态轮询 |
| 相机集成 | 奥比中光 Orbbec 3D 相机，Python 子进程流媒体显示 |
| 设备管理 | IP 配置面板、TCP 连通性测试、补光灯 GPIO 控制（Linux） |
| 主题系统 | 深色 / 浅色双主题，动画切换开关，修复 Ubuntu 白色背景问题 |
| 修改日志 | `changelog/CHANGELOG.md` 记录每次改动详情 |

---

## 五步工作流时序

```
Step 0  相机拍照   →  触发 Orbbec 3D 采集，等待 XYZ 坐标返回
Step 1  机器人抓取 →  写 Holding 901-906（坐标）→ 写 Holding 900=8（抓取） → 轮询 Input 1132==2
Step 2  AGV 行走   →  写 AGV Holding 1000（目标工位）→ 轮询 AGV Input 1001==2（到位）
Step 3  机器人放料 →  写 Holding 900=9（翻转卸料）→ 轮询 Input 1132==3
Step 4  复位等待   →  写 Holding 900=0（复位）→ 轮询 Input 1132==0（空闲）
         └─ 循环，工位 1→2→…→12→1，12 个工位巡回
```

> **仿真模式**：任何设备未连接时，对应步骤自动按设定延时推进，无需真实硬件即可演示全流程。

---

## Modbus 寄存器速查

### 华沿机械臂（默认 192.168.10.10:502）

| 地址 | 类型 | 说明 |
|------|------|------|
| Holding 900 | 读写 | CmdID：8=抓取，9=翻转卸料，0=复位 |
| Holding 901-906 | 读写 | X / Y / Z / Rx / Ry / Rz 坐标 |
| Input 1132 | 只读 | 状态字：0=空闲，1=请求拍照，2=抓取完成，3=卸料完成 |

### 仙工 AGV（默认 192.168.192.5:502）

| 地址 | 类型 | 说明 |
|------|------|------|
| Holding 1000 | 读写 | 目标工位编号 |
| Input 1001 | 只读 | 状态字：0=空闲，1=行走中，2=已到位，3=故障 |

> ⚠️ 以上地址为项目默认值，部署前请对照各设备接口手册验证。

---

## 项目结构

```
wh-robot-visual/
├── CMakeLists.txt          # 构建配置（Qt6, C++17, 跨平台）
├── main.cpp
├── mainwindow.h/cpp        # 主窗口（纯 UI，不含业务逻辑）
├── mainwindow.ui
│
├── workflowengine.h/cpp    # 工作流状态机（不依赖 UI）
├── devicemanager.h/cpp     # 设备管理器（配置/测试/补光灯）
├── robotcontroller.h/cpp   # 华沿机械臂 Modbus TCP 客户端
├── agvcontroller.h/cpp     # 仙工 AGV Modbus TCP 客户端
│
├── camerawindow.h/cpp      # 奥比相机实时画面窗口
├── workflowwidget.h/cpp    # 工作流步骤可视化控件
├── deviceindicator.h/cpp   # 设备状态指示灯控件
├── themeswitch.h/cpp       # 深色/浅色主题切换开关控件
├── workflowstep.h          # 步骤数据结构
│
├── camera_stream.py        # 奥比相机 Python 流媒体脚本
├── camera_test.py          # 相机测试脚本
├── fill_light              # 补光灯 GPIO 控制程序（Linux）
│
└── changelog/
    └── CHANGELOG.md        # 代码修改记录
```

---

## 构建环境

| 项目 | 要求 |
|------|------|
| Qt | 6.x（已验证 6.8.3 / 6.9.3） |
| 必需模块 | `Qt6::Widgets` `Qt6::Network` `Qt6::SerialBus` |
| C++ 标准 | C++17 |
| 编译器（Windows） | MinGW 或 MSVC |
| 编译器（Linux） | GCC 9+ |
| CMake | 3.16+ |

### Windows（开发环境）

```bash
# 用 Qt Creator 打开 CMakeLists.txt
# 选择 Qt 6.x MinGW 或 MSVC kit，点击 Build
```

> Linux 专属功能（Orbbec SDK 链接、GPIO 补光灯）在 Windows 编译时自动跳过，不影响 UI 调试。

### Linux（部署环境）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

相机功能需要 Python 环境：

```bash
# 安装奥比 Python SDK
pip install pyorbbecsdk

# 或使用 miniconda（程序自动检测 /home/agv/miniconda3/bin/python3）
```

---

## 快速上手

1. **配置 IP**：在左侧面板填写机器人 IP / AGV IP，点击"应用配置"
2. **测试连接**：各设备旁的"测试"按钮验证 TCP 可达性
3. **设置延时**：工具栏调整步进延时（无硬件时为每步仿真时长）
4. **开始运行**：点击"▶ 开始运行"启动五步自动循环
5. **单步调试**：点击"▶| 单步运行"手动逐步推进

---

## 架构说明

```
MainWindow（UI 层）
    ├── WorkflowEngine   业务状态机，不含任何 UI 代码
    └── DeviceManager    设备生命周期管理
            ├── RobotController   机械臂 Modbus TCP
            └── AgvController     AGV Modbus TCP
```

- **UI ↔ 业务完全解耦**：通过 Qt 信号/槽通信，互不依赖头文件
- **双模式运行**：同一套代码，有硬件走真实 Modbus，无硬件自动降级仿真
- **跨平台**：`#ifdef Q_OS_LINUX` 隔离 GPIO / Orbbec SDK 等 Linux 专属调用

---

## License

内部项目，暂不开源授权。
