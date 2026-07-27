# 智能体协作规则

## 硬件环境

- Ubuntu 虚拟机，内存 8GB。

## 子代理规则

- 只有在用户明确要求或技能明确要求时，才使用子代理。
- 使用 `subagent-driven-development` 时，请询问我串行执行还是并行执行，因为我不同电脑性能不一样。
- 同一时间最多运行一个子代理，也请事先询问我，因为我不同电脑性能不一样。
- 禁止派发并行子代理也请事先询问我，因为我不同电脑性能不一样。
- 除非任务明确要求，子代理不要自行运行构建。
- 构建和测试由主代理统一控制。

## 文档语言规则
- 生成的md文档命名中需要附带当天的日期，以便后续追溯
- 生成的md文档内容都需要用中文书写，不要用英文
- 生成的具体代码文件中，接口、变量、复杂逻辑等要添加详细的注释，以便后续理解代码具体功能实现
- 面向人的说明性文档默认使用中文，包括计划、规格、审查说明、`AGENTS.md` 的规则文件。
- 代码标识符、类名、函数名、宏名、测试目标名、命令、路径和第三方库名称保持原样，不强行翻译。
- 新增或修改计划、规格文档时，正文标题、步骤说明、预期结果、自查说明必须使用中文。
- 如果引用英文报错、SDK 原文或命令输出，应保留原文，并在需要时补充中文解释。
- 修复的bug，必须要在对应的md中详细记录，不能只修改代码没有过程记录，如果没有对应的md，就新建md，以便后续追溯
- push之前需要更新README.md相应内容,以及需要更新CHANGELOG.md
- 代码可以commit,大功能整体提交一次，提交内容也要用中文，但是请不要私自push，要征求我同意

# `wh-robot-visual` 智能体编码指南

本仓库是一个 Qt 6 / C++17 桌面应用，用于仓储机器人可视化和控制。

## 项目是什么

- Qt 6 应用，使用 CMake 构建。
- 支持 Linux 和 Windows。
- 通过 Modbus TCP 与硬件通信：
  - 华沿机械臂（`RobotController`）
  - 仙工 AGV（`AgvController`）
- 通过 HTTP 与视觉服务通信（`VisionHttpClient`）。
- 设备未连接时支持硬件仿真兜底。

## 关键文件和模块

- `CMakeLists.txt`：主构建文件，使用 `qt_add_executable`。
- `src/mainwindow.*`：界面布局和事件转发。
- `src/workflowengine.*`：核心五步自动化状态机。
- `src/devicemanager.*`：设备生命周期、连接状态和灯控。
- `src/robotcontroller.*`：机械臂 Modbus TCP 客户端。
- `src/agvcontroller.*`：AGV Modbus TCP 客户端。
- `src/visionclient.*`：视觉推理 HTTP 客户端和坐标转换。
- `src/handeyedialog.*`：手眼标定矩阵加载。
- `src/camerawindow.*`：相机预览窗口。
- `src/deviceindicator.*`：状态指示控件。
- `src/workflowwidget.*`：流程可视化控件。

## 构建和运行

### Linux

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Windows

- 使用 Qt Creator 打开 `CMakeLists.txt`。
- 使用 Qt 6.8+ 套件，编译器可选 MinGW 或 MSVC。
- Windows 下会跳过仅 Linux 支持的 Orbbec SDK 链接。

## 重要仓库约定

- C++ 风格遵循 `docs/dh_code_style.md`。
- 使用 4 空格缩进，Qt 风格的 `if (condition) { ... }` 格式。
- 成员变量使用 `m_` 前缀。
- 类名使用 `PascalCase`；函数名使用 `camelCase`。
- 信号使用过去式、被动语义或 `Changed` 后缀。
- 槽函数通常使用 `on...` 前缀。

## 修改时优先保持的边界

- 界面代码保留在 `mainwindow`，业务逻辑保留在 `workflowengine`。
- Modbus 和硬件通信保留在对应 controller 类中。
- 视觉 HTTP 和坐标转换保留在 `visionclient`。
- 避免把控制流程状态机逻辑混入 `MainWindow`。

## 需要保留的项目细节

- 项目设计同时支持真实硬件和仿真兜底两种模式。
- Orbbec SDK 只在 Linux 支持，依赖 `3rd/OrbbecSdk`。
- `Log/` 是运行时自动创建的日志目录，除非明确要求，不要提交其中变更。
- `src/workflowstep.h` 和流程引擎是五步循环的核心。
- 手眼变换是 4×4 矩阵，由 `HandEyeDialog` 加载并传给视觉客户端。

## 常用文档

- [`README.md`](README.md)：项目概览和快速开始。
- [`docs/dh_project.md`](docs/dh_project.md)：架构和模块说明。
- [`docs/dh_code_style.md`](docs/dh_code_style.md)：编码约定。
- [`changelog/CHANGELOG.md`](changelog/CHANGELOG.md)：变更历史。

## 给智能体的操作建议

- 优先做小而明确的增量修改，避免大范围重写。
- 修复流程行为时，优先检查 `src/workflowengine.cpp`。
- 修复硬件集成时，优先检查 `src/robotcontroller.cpp`、`src/agvcontroller.cpp` 和 `src/visionclient.cpp`。
- 增加界面行为时，优先检查 `src/mainwindow.cpp` 和 `src/mainwindow.ui`。
- 尊重现有构建系统，不要假设项目可以脱离 Qt 构建。

## 常见风险

- Orbbec SDK 从源码树链接，并且只在 `UNIX` 为真时支持。
- 部分设备行为依赖预定义 Modbus 寄存器值；没有硬件文档确认时，不要修改寄存器常量。
- `VisionHttpClient` 负责 901-906 寄存器的坐标转换；这里的错误会影响整条机器人流程。
- `Log/` 是运行时生成目录，除非明确要求，不要提交其中内容。
