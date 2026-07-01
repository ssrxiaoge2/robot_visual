# 仓储机器人上位机可视化系统

基于 **Qt 6 + C++17** 开发的工业仓储自动化上位机软件，运行于 Windows / Linux 工控机。机械臂通过 **华沿 SDK** 控制，AGV 通过 **Modbus TCP** 控制，视觉服务通过 **HTTP** 通信，扫码枪通过 **N-ScanHub SDK / TCP** 主动触发读取。

当前主流程由 `LineManager + TaskExecutor` 驱动：现场 12 个工位发生缺料后进入 FIFO 队列，系统依次完成 AGV 前往取料位、机械臂视觉取料、夹紧前扫码、AGV 前往倒料位、机械臂倒料、AGV 前往码垛位、机械臂放置空箱，队列为空时 AGV 回到 LM1 待机。

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
│   ├── linemanager.{h,cpp}         # 12 工位连续补料 FIFO 调度入口
│   ├── taskexecutor.{h,cpp}        # 单个缺料任务状态机
│   ├── taskqueue.h                 # 进程内 Pending 任务 FIFO 队列
│   ├── lineconfig.h                # 12 工位 LM 点位与示教函数配置表
│   ├── lineorchestrator.{h,cpp}    # 旧单工位流程参考，当前不作为主流程入口
│   ├── huayanScheduler.{h,cpp}     # 华沿机械臂 SDK 调度（取料/收姿态/倒料阶段机）
│   ├── agvcontroller.{h,cpp}       # 仙工 AGV Modbus TCP（监控轮询 + 派单）
│   ├── visionclient.{h,cpp}        # 视觉 HTTP + 手眼坐标转换 + 多目标择优
│   ├── devicemanager.{h,cpp}       # 设备生命周期管理 + 工位→站点映射
│   ├── nscanscheduler.{h,cpp}      # N-ScanHub 网络扫码 SDK 同步封装
│   ├── palletscheduler.{h,cpp}     # 空箱码垛点位规划与已放数量缓存
│   ├── palletparamdialog.{h,cpp}   # 码垛参数配置窗口
│   ├── customSysScheduler.{h,cpp}  # 客户系统 REST API 连通性和日统计读取
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
3. 点击「Start」或顶部「▶ 开始运行」启动 `LineManager`，系统进入“等待缺料”
4. 在「调度监控」中点击「工位1」到「工位12」模拟缺料，任务按 FIFO 顺序执行
5. 点击「Stop」执行急停语义：取消 AGV、停止机械臂、清空 Pending 队列并进入 Error
6. 现场处理完成后点击「Reset Error」恢复到 Idle，再重新 Start
7. AGV 调试面板可单独派单 / 取消 / 暂停 / 继续，便于联调

---

## 12 工位缺料流程

`LineManager` 是当前整线入口。它只负责队列、启停、回 LM1 和 Error 边界；单个工位的动作由 `TaskExecutor` 串行执行。系统启动后没有任务时保持“等待缺料”，收到缺料事件后创建一个任务，多个工位同时缺料时按 FIFO 依次执行。

```
缺料入队       工位按钮 / 后续客户系统事件 → TaskQueue FIFO
  ↓
AGV→取料位     下发该工位 pickupLm，监控 navStatus/navStation/curStation 判到达
  ↓
机械臂取料     调该工位 Func_captureN 到拍照/取料安全位，视觉闭环对准
  ↓
夹紧前扫码     N-ScanHub 主动扫码；首轮失败后机械臂按工具系 Y 轴依次搜索 +20 mm、-20 mm
  → 搜码结果       扫到码则先回原夹取位继续夹紧；两次搜码都失败则回拍照位并按任务失败收尾
  ↓
夹紧抬升       扫码通过后机械臂继续夹紧、抬升
  ↓
收姿态         调 Func_yun_xing_zhong 回运行中安全姿态
  ↓
AGV→倒料位     下发该工位 unloadLm；如果 pickupLm == unloadLm，则跳过同站导航
  ↓
机械臂倒料     调该工位 Func_daoliaoN 到倒料准备位，再调 Func_fanzhuan 翻转倒料
  ↓
收姿态         倒料后再次回运行中安全姿态
  ↓
AGV→码垛位     大多数工位到共享码垛区 LM16；工位12 到独立码垛区 LM17
  ↓
空箱码垛       PalletScheduler 计算下一相对偏移，机械臂到码垛基准位并松爪
  ↓
提交缓存       放置成功后 commitPlaced() 推进已放数量
  ↓
最终收姿态     任务完成；有下一任务则继续 FIFO，没有任务则 AGV 回 LM1 待机
```

### 工位点位与示教函数

`src/lineconfig.h` 是 12 个工位的固定配置来源。表中的 LM 是上位机直接下发并用于到达判定的数字站点；LM2 这类中间过渡点属于 AGV 地图路径规划内部节点，通常不由上位机单独派单。以工位1为例：AGV 从 LM1 待机点出发，地图路径可能经过 LM2，最终到达上位机下发的 LM3；到达 LM3 后机械臂调用 `Func_capture1` 完成拍照位/取料流程，取完料后因取料位和倒料位同为 LM3，跳过二次导航，直接调用 `Func_daoliao1` 到倒料准备位，再执行 `Func_fanzhuan` 完成倒料。

| 工位 | 取料 LM | 倒料 LM | 拍照/取料函数 | 倒料准备函数 | 倒料函数 | 空箱码垛区 |
|------|---------|---------|---------------|--------------|----------|------------|
| 1 | LM3 | LM3 | `Func_capture1` | `Func_daoliao1` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 2 | LM4 | LM4 | `Func_capture2` | `Func_daoliao2` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 3 | LM5 | LM9 | `Func_capture3` | `Func_daoliao3` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 4 | LM5 | LM9 | `Func_capture4` | `Func_daoliao4` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 5 | LM5 | LM10 | `Func_capture5` | `Func_daoliao5` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 6 | LM6 | LM10 | `Func_capture6` | `Func_daoliao6` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 7 | LM6 | LM11 | `Func_capture7` | `Func_daoliao7` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 8 | LM6 | LM11 | `Func_capture8` | `Func_daoliao8` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 9 | LM7 | LM12 | `Func_capture9` | `Func_daoliao9` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 10 | LM7 | LM12 | `Func_capture10` | `Func_daoliao10` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 11 | LM7 | LM7 | `Func_capture11` | `Func_daoliao11` | `Func_fanzhuan` | 共享码垛区 LM16 |
| 12 | LM15 | LM15 | `Func_capture12` | `Func_daoliao12` | `Func_fanzhuan` | 工位12独立码垛区 LM17 |

### 失败处理

- AGV 每段导航有 120s 超时保护；到达判定必须同时满足 `navStatus=Arrived`、`navStation=目标 LM`、`curStation=目标 LM`。
- 机械臂任一阶段失败时，当前任务先尝试执行安全收姿态；收姿态成功则该任务失败但 FIFO 可以继续。
- 视觉计算出的 Rz 若出现 80° 以上大角度跳变，必须连续两帧同向确认后才允许执行旋转，避免 `minAreaRect()` 偶发 90° 跳变直接驱动机械臂误转。
- 夹紧前扫码首轮失败后，机械臂按工具系 Y 轴 `+20 mm`、`-20 mm` 依次搜码；这是当前默认的 Y 轴搜码补救流程。搜到后回原夹取位继续夹紧，两次都失败则回拍照位并按任务失败收尾。
- 扫码失败默认不执行 180 度旋转补救，避免现场碰撞风险；当前任务失败并安全收姿态。
- 系统级错误、AGV 通信错误、码垛缓存提交失败、急停 Stop 会取消 AGV、停止机械臂、清空 Pending 队列并进入 Error，必须人工 Reset Error。

**机械臂阶段机**（`HuayanScheduler`）：`StageOne`（视觉定位、夹紧前扫码暂停、夹紧抬升）、`Stow`（收姿态）、`Unload`（倒料）、`PalletPlace`（空箱码垛独立动作），均通过华沿 SDK `HRIF_RunFunc` / `HRIF_MoveRelL` 等下发，以 100ms 轮询机器人运动标志判完成。

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
| 调度监控看板 | 完成 | Start / Stop / Reset Error，12 工位缺料模拟，当前任务与 FIFO 队列展示 |
| 12 工位 FIFO 补料 | 完成 | `LineManager + TaskExecutor` 串行执行缺料任务，队列为空回 LM1 |
| 华沿 SDK 机械臂调度 | 完成 | 取料（视觉闭环 + 夹紧前扫码）/ 收姿态 / 倒料 / 空箱码垛 |
| AGV Modbus 监控 + 派单 | 完成 | 监控轮询 + 工位→站点映射 + 调试面板 |
| AGV 严格到达判定 | 完成 | 同时校验导航状态、导航目标 LM 和当前物理 LM，避免残留到达误判 |
| 视觉 HTTP + 坐标转换 | 完成 | /inference + 手眼矩阵 + 多目标择优 |
| 相机实时预览 | 完成 | /frame/annotated JPEG 流 |
| N-ScanHub 主动扫码 | 完成 | 调试扫码与主调度扫码使用不同 worker，SDK 入口用 mutex 串行保护 |
| 空箱码垛规划 | 完成 | 大箱/小箱两套区域配置、下一点位计算、已放数量缓存 |
| 客户系统通信测试 | 完成 | REST API 连通性测试，读取日统计并提取 actualQty |
| 手眼矩阵加载向导 | 完成 | 文件加载 / 手动填写 |
| 补光灯控制 | 完成 | Linux GPIO，Windows 静默跳过 |
| 日志持久化 | 完成 | Log/<日期> log.txt |
| 整线真机试跑 | 进行中 | 已修复多处真机问题，仍需现场验证 |
| 客户系统自动入队 | 未实现 | 当前缺料入口仍以 UI 模拟按钮为主，客户系统 actualQty 暂未驱动 FIFO |
| M4 车队调度 / 多车互斥 | 设计中 | 设计文档已存在，源码当前仍是 SEER Modbus 单 AGV 主线 |

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
    ├── 调度监控看板 / 设备调试面板 / 码垛配置面板
    └── DeviceManager        设备生命周期管理、跨线程 worker 和信号接线中心
            ├── LineManager       12 工位 FIFO 整线状态机
            │     ├── TaskQueue       Pending 任务队列
            │     └── TaskExecutor    单个缺料任务状态机
            ├── AgvController     仙工 AGV Modbus TCP
            ├── HuayanScheduler   华沿机械臂 SDK 调度
            ├── VisionHttpClient  视觉 HTTP + 手眼坐标变换
            ├── NScanScheduler    N-ScanHub 主动扫码
            ├── PalletScheduler   空箱码垛规划与缓存
            └── CustomSysScheduler 客户系统 REST API 测试
```

核心原则：

- **MainWindow** 只做布局和信号转发，不含 Modbus / SDK / 网络逻辑
- **DeviceManager** 是设备对象唯一所有者；调度扫码和 UI 测试扫码隔离，避免结果串线
- **LineManager** 只管理整线状态、FIFO 队列、回 LM1 和 Error 边界，不直接调用 Modbus/SDK 原语
- **TaskExecutor** 只执行一个任务的高层动作，所有硬件细节委托给 AGV、机械臂、视觉、扫码、码垛模块
- **控制器**（`HuayanScheduler` / `AgvController` / `VisionHttpClient` / `NScanScheduler` 等）由 `DeviceManager` 持有，不在别处 new

---

## 待办与待确认事项

| 事项 | 说明 |
|------|------|
| 站点映射核对 | 取料/倒料/待机工位对应的 AGV 物理站点号需与现场地图一致 |
| 12 工位示教函数确认 | `Func_capture1` 到 `Func_capture12`、`Func_daoliao1` 到 `Func_daoliao12`、`Func_fanzhuan`、码垛基准函数和 `Func_songzhua` 须在示教器中已示教 |
| 码垛 LM16/LM17 核对 | 当前为共享码垛区和工位12独立码垛区配置，投产前需与现场地图一致 |
| baseRzReg 基准 | `VisionHttpClient` 默认 0，需联机后设实际值 |
| 手眼标定精度 | 当前误差约 2–3mm，必要时重标后 `setHandEyeMatrix()` 更新 |
| 叉车货叉扩展 | 取放货需扩展货叉高度 / 到位寄存器 |
| 客户系统自动派单 | 当前只做 REST 连通性和 actualQty 读取，缺料事件自动生成任务的规则待确认 |
| M4 车队调度 | 多车互斥和外部地图资源锁仍是设计阶段，当前生产主线不依赖它 |

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
