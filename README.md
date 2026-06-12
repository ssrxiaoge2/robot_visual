# 仓储机器人上位机可视化系统

基于 **Qt 6 + C++17** 开发的工业仓储自动化上位机软件，运行于 Windows / Linux 工控机。机械臂通过 **华沿 SDK** 控制，AGV 通过 **Modbus TCP** 控制，视觉服务通过 **HTTP** 通信，由 `LineOrchestrator` 串起 AGV 取料 → 视觉夹取 → 运输 → 倒料 → 返回的整线自动化循环。

**GitHub**：https://github.com/ssrxiaoge2/robot_visual.git

---

## 硬件配置

| 设备 | 型号 | 通信协议 | 默认地址 |
|------|------|----------|---------|
| 协作机械臂 | 华沿 E05 / E10 / S60 | 华沿 SDK（TCP） | 192.168.1.11:10003 |
| 移动机器人 | 仙工（SEER）AGV / 叉车 | Modbus TCP | 192.168.1.100:502 |
| 深度相机 | 奥比中光 Femto / Astra | HTTP (Flask) | 8080 端口 |
| 扫码器 | 通用 TCP | TCP | 192.168.1.12 |
| 补光灯 | GPIO 控制 (PCF9554) | Linux GPIO | 仅 Linux |

> 手眼标定为 Eye-in-Hand（相机固定于机械臂末端），标定矩阵存于 `hand-eye/calibration_output/handeye_matrix.txt`，已内置为 `VisionHttpClient` 默认值。

---

## 软件依赖

| 环境 | 要求 |
|------|------|
| Qt | 6.8+ |
| Qt 模块 | `Widgets` `Network` `SerialBus`（Modbus） |
| C++ | C++17 |
| CMake | 3.16+ |
| 编译器（开发）| MinGW 64-bit 或 MSVC 2022（Windows） |
| 编译器（部署）| GCC 9+（Linux） |
| 华沿 SDK | `3rd/HuaYansdk/HuayanRobotLibrary-C++-V1.0.15.0`（随仓库提供） |
| Orbbec SDK | 仅 Linux 部署链接（`if(UNIX)` 条件编译） |
| Python（相机）| 3.8+，pyorbbecsdk，Flask，ultralytics，sam2 |

---

## 项目结构

```
wh-robot-visual/
├── CMakeLists.txt
├── 3rd/HuaYansdk/                  # 华沿机器人 C++ SDK（库 + 头文件）
├── src/
│   ├── main.cpp
│   ├── mainwindow.{h,cpp}          # 纯 UI 层：布局 + 信号转发
│   ├── lineorchestrator.{h,cpp}    # 整线流程编排器（AGV + 机械臂时序协调）
│   ├── huayanScheduler.{h,cpp}     # 华沿机械臂 SDK 调度（取料/收姿态/倒料阶段机）
│   ├── agvcontroller.{h,cpp}       # 仙工 AGV Modbus TCP（监控轮询 + 派单）
│   ├── visionclient.{h,cpp}        # 视觉 HTTP + 手眼坐标转换 + 多目标择优
│   ├── devicemanager.{h,cpp}       # 设备生命周期管理 + 工位→站点映射
│   ├── camerawindow.{h,cpp}        # 相机实时预览对话框
│   ├── handeyedialog.{h,cpp}       # 手眼矩阵加载向导
│   ├── deviceindicator.{h,cpp}     # LED 状态指示灯控件
│   ├── workflowwidget.{h,cpp}      # 流程图可视化控件
│   ├── workflowstep.h              # 流程步骤元数据
│   └── themeswitch.{h,cpp}         # 深色/浅色主题切换
├── scripts/
│   └── agv_modbus_debug.py         # AGV Modbus 真机调试脚本
├── Log/                            # 运行日志（自动创建，yyyy-MM-dd log.txt）
├── changelog/CHANGELOG.md
└── docs/superpowers/               # 设计文档与实现计划
```

**视觉服务**（同仓库子目录）：

```
View_data_http/
└── main_angle_depth_samseg_depth_http.py   # Flask：YOLO + SAM2 + Orbbec RGB-D
hand-eye/calibration_output/handeye_matrix.txt   # 手眼标定矩阵（4×4 行主序）
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

> 运行时需把 `3rd/HuaYansdk/.../MinGW`（含 `libHR_Pro.dll`）加入 PATH，否则启动报缺少 DLL。
> Linux 专属功能（Orbbec SDK、GPIO 补光灯）在 Windows 编译时自动跳过。

### 3. 编译（Linux 部署）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 4. 运行视觉服务（Linux 工控机）

```bash
cd View_data_http
python3 main_angle_depth_samseg_depth_http.py   # Flask 在 8080 端口监听
```

### 5. 使用上位机

1. 填写机械臂 / AGV / 视觉各设备 IP，点击「应用配置」建立连接
2. 用各设备旁「测试」按钮验证 TCP 可达性
3. 点击「▶ 开始运行」启动整线流程；点击「■ 停止」就地停机
4. AGV 调试面板可单独派单 / 取消 / 暂停 / 继续，便于联调

---

## 整线流程

`LineOrchestrator` 实现单次（非自动循环）的整线状态机，任一步出错就地停机告警（取消 AGV 导航 + 停机械臂）：

```
初检       AGV 须在待机站（真检测 curStation）+ 强制机械臂收姿态
  ↓
AGV→取料站  派工位号 → 经映射表转物理站点号 → 监控轮询判到达
  ↓
机械臂取料  华沿 SDK：拍照位 → 视觉闭环对准(XY+Rz) → Z 下探 → 夹紧 → 抬升
  ↓
收姿态      调 Func_yun_xing_zhong 收回运行姿态
  ↓
AGV→倒料站  派站 + 到达判定
  ↓
倒料        调 Func_daoliao_1_point → Func_daoliao
  ↓
收姿态 → AGV→待机站 → 完成
```

**机械臂阶段机**（`HuayanScheduler`）：`StageOne`（取料，含视觉闭环迭代）、`Stow`（收姿态）、`Unload`（倒料），均通过华沿 SDK `HRIF_RunFunc` / `HRIF_MoveRelL` 等下发，以 100ms 轮询机器人运动标志判完成。

**机械臂未连接时**直接报连接失败；AGV 未连接时整线初检不通过——整线流程需真实硬件在线。

---

## 视觉服务 HTTP API

| 端点 | 方法 | 用途 |
|------|------|------|
| `/inference` | GET | 目标检测，返回 `offset_mm(x,y)`、`depth_compensated`、`angle` |
| `/frame/annotated` | GET | 带标注框的 JPEG（供预览窗口实时显示） |
| `/status` | GET | 连通性检测，含 `camera_running` 字段 |

**坐标转换**（`VisionHttpClient::transformToMm`）：相机系偏移 → 手眼矩阵 T_tool_cam → 工具系(mm) → ×1000 寄存器值；Rz = baseRzReg + 规范化角度（夹爪 180° 对称）。

**多目标择优**：深度最小（离相机最近 / 堆叠最上层）优先；深度差小于同层容差 `kSameLayerTolMm`(20mm) 时视为并排平放，改取离图像中心最近者。

---

## AGV Modbus 寄存器速查（仙工 SEER）

> 文档地址位以 00001 为起始，发送请求时统一减 1 转 0 基 PDU 地址（`AgvController::pdu()`）。

| 类型 | 文档地址 | 读写 | 说明 |
|------|---------|------|------|
| Holding [4x] | 00001 | 写 | 目标站点 id，写入 >0 触发导航，AGV 收到后自动清 0 |
| Input [3x] | 00007 | 读 | 当前导航站点 id |
| Input [3x] | 00008 | 读 | 定位状态：0 失败 / 1 正确 / 2 重定位中 / 3 完成 |
| Input [3x] | 00009 | 读 | 导航状态：0 无 / 1 等待 / 2 执行 / 3 暂停 / 4 到达 / 5 失败 / 6 取消 / 7 超时 |
| Input [3x] | 00034 | 读 | 当前所在站点 id（到达判定用） |
| Input [3x] | 00043 | 读 | 控制权是否被外部抢占：0 否 / 1 是 |
| Coil [0x] | 00006 | 写 | 取消导航，写 1 触发，自动清 0 |

> 上位机以**工位号**派单，经 `DeviceManager` 映射表转为 AGV **物理站点号**下发；监控回报为物理站点号，到达判定在同一空间比较。

---

## 当前功能状态

| 功能 | 状态 | 说明 |
|------|------|------|
| UI 布局 + 深浅色主题 | 完成 | 2×2 设备状态网格，动画主题切换 |
| 华沿 SDK 机械臂调度 | 完成 | 取料（视觉闭环）/ 收姿态 / 倒料阶段机 |
| AGV Modbus 监控 + 派单 | 完成 | 监控轮询 + 工位→站点映射 + 调试面板 |
| 整线流程编排（单次） | 完成 | AGV 取料 → 夹取 → 倒料 → 返回，出错就地停机 |
| 视觉 HTTP + 坐标转换 | 完成 | /inference + 手眼矩阵 + 多目标择优 |
| 相机实时预览 | 完成 | /frame/annotated JPEG 流 |
| 手眼矩阵加载向导 | 完成 | 文件加载 / 手动填写 |
| 补光灯控制 | 完成 | Linux GPIO，Windows 静默跳过 |
| 日志持久化 | 完成 | Log/<日期> log.txt |
| 整线真机试跑 | 进行中 | 已修复多处真机问题，仍需现场验证 |
| 连续循环 / 多站点 | 未实现 | 当前为单次单站点试测 |
| 扫码器数据读取 | 未实现 | 仅 TCP 连通性测试 |
| MES 接口 | 未实现 | API 文档待提供 |

---

## 真机试跑已修复问题

| 问题 | 修复 |
|------|------|
| 收姿态/倒料函数 20601（函数不存在） | 函数名首字母改大写 `Func_*`，对齐示教器已示教名 |
| 取料站到达不触发拍照 | 到达判定按物理站点号比较（注入 `resolveStation` 解析器） |
| 阶段衔接 20018（ProgramStopped） | `GrpReset` 后延时 500ms 再下发首条指令 |
| 取料闭环 Rz 持续旋转累积 180° | 旋转方向取反 `addMove(5, -rz)`（眼在手上闭环符号反向） |
| 取料后收姿态不动 | 阶段收尾先 `stop()` 再发完成信号，避免重置刚启动的下一阶段 |

---

## 架构说明

```
MainWindow（UI 层）
    ├── LineOrchestrator     整线流程状态机（纯协调层，不含 Modbus/SDK 原语）
    └── DeviceManager        设备生命周期管理 + 工位→站点映射
            ├── HuayanScheduler   华沿机械臂 SDK 调度
            ├── AgvController     仙工 AGV Modbus TCP
            └── VisionHttpClient  视觉 HTTP + 手眼坐标变换
```

核心原则：

- **MainWindow** 只做布局和信号转发，不含 Modbus / SDK / 网络逻辑
- **LineOrchestrator** 纯协调层，只调高层方法并监听完成信号，不触碰 Modbus/SDK 原语
- **控制器**（`HuayanScheduler` / `AgvController` / `VisionHttpClient`）由 `DeviceManager` 持有，不在别处 new

---

## 待办与待确认事项

| 事项 | 说明 |
|------|------|
| 站点映射核对 | 取料/倒料/待机工位对应的 AGV 物理站点号需与现场地图一致 |
| 倒料函数确认 | `Func_daoliao_1_point` / `Func_daoliao` 须在示教器中已示教 |
| baseRzReg 基准 | `VisionHttpClient` 默认 0，需联机后设实际值 |
| 手眼标定精度 | 当前误差约 2–3mm，必要时重标后 `setHandEyeMatrix()` 更新 |
| 叉车货叉扩展 | 取放货需扩展货叉高度 / 到位寄存器 |
| 扫码器 / MES | 待协议与 API 文档 |

---

## 开发规范

详见 `CLAUDE.md`，要点：

- 分支策略：`feature/<名称>` 开发，PR 合并到 `master`，不直接 push master
- Commit 格式：`feat:` / `fix:` / `refactor:` / `docs:` 前缀
- 每次 PR 前更新 `changelog/CHANGELOG.md`
- 中文字符串一律 `QStringLiteral`，无 emoji，新增有颜色控件在 `applyTheme(bool dark)` 中同步适配

---

## License

内部项目，暂不开源。
