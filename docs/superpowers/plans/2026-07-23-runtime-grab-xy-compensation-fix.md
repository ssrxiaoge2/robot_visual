# 运行时抓取 X/Y 补偿修复实施计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:executing-plans 逐任务实施本计划。步骤使用复选框（`- [ ]`）语法跟踪进度。

**目标：** 让设置中保存的抓取 X/Y 补偿值从下一次抓取开始实际控制机械臂相对运动。

**架构：** 保留现有设置持久化和运行时快照分发架构，仅修正 `HuayanScheduler` 阶段一抓取补偿的数据源。先用契约测试证明执行路径仍依赖硬编码常量，再将判零、日志、方向和距离统一切换为运行时设置字段。

**技术栈：** C++17、Qt 6、CMake、CTest、MSVC

---

## 文件结构

- 修改：`tests/test_huayan_scheduler_contract.cpp`  
  增加抓取 X/Y 补偿必须读取运行时设置且不得保留硬编码常量的回归契约。
- 修改：`src/huayanScheduler.cpp`  
  删除 X/Y 补偿编译期常量，并在阶段一抓取补偿状态机中读取 `RuntimeSettings`。

### 任务 1：建立失败的 X/Y 补偿运行时契约

**文件：**

- 测试：`tests/test_huayan_scheduler_contract.cpp`

- [ ] **步骤 1：添加失败的契约断言**

在现有运行时设置断言之后加入：

```cpp
requireTrue(source.contains(
                QStringLiteral("m_runtimeSettings.pickup.grabXCompensationMm")),
            "抓取 X 补偿必须来自运行时设置快照");
requireTrue(source.contains(
                QStringLiteral("m_runtimeSettings.pickup.grabYCompensationMm")),
            "抓取 Y 补偿必须来自运行时设置快照");
requireTrue(!source.contains(QStringLiteral("kGrabXCompensation")),
            "抓取 X 补偿不得继续使用编译期常量");
requireTrue(!source.contains(QStringLiteral("kGrabYCompensation")),
            "抓取 Y 补偿不得继续使用编译期常量");
```

- [ ] **步骤 2：构建并运行定向测试，确认红灯**

运行：

```powershell
cmd.exe /c "call D:\study\VS\community\VC\Auxiliary\Build\vcvars64.bat && D:\study\qt\Tools\CMake_64\bin\cmake.exe --build build-codex-msvc-release --config Release --target huayan_scheduler_contract_tests"
$env:Path='D:\study\qt\6.8.3\msvc2022_64\bin;' + $env:Path
$env:QT_PLUGIN_PATH='D:\study\qt\6.8.3\msvc2022_64\plugins'
$env:QT_QPA_PLATFORM='offscreen'
D:\study\qt\Tools\CMake_64\bin\ctest.exe --test-dir build-codex-msvc-release -C Release -R "^huayan_scheduler_contract_tests$" --output-on-failure
```

预期：测试失败，错误指出抓取 X 或 Y 补偿没有来自运行时设置，或仍存在编译期常量。

### 任务 2：将抓取补偿切换到运行时设置

**文件：**

- 修改：`src/huayanScheduler.cpp`
- 测试：`tests/test_huayan_scheduler_contract.cpp`

- [ ] **步骤 1：删除硬编码常量**

删除：

```cpp
static constexpr double kGrabXCompensation = 30.0;
static constexpr double kGrabYCompensation = -17.0;
```

- [ ] **步骤 2：在执行路径读取运行时设置**

在处理抓取 Z 下探、X 补偿和 Y 补偿的代码块中先绑定当前快照值：

```cpp
const double grabXCompensation =
    m_runtimeSettings.pickup.grabXCompensationMm;
const double grabYCompensation =
    m_runtimeSettings.pickup.grabYCompensationMm;
```

随后统一使用 `grabXCompensation` 和 `grabYCompensation` 完成：

- 与 `kOffsetIgnoreDistance` 的比较；
- 日志参数；
- 正负方向选择；
- 绝对运动距离。

- [ ] **步骤 3：构建并运行定向测试，确认绿灯**

重复任务 1 的定向构建与 CTest 命令。

预期：`huayan_scheduler_contract_tests` 通过。

- [ ] **步骤 4：提交修复**

```powershell
git add -- tests/test_huayan_scheduler_contract.cpp src/huayanScheduler.cpp
git commit -m "fix: apply runtime XY grab compensation"
```

### 任务 3：全量验证

**文件：**

- 验证：`CMakeLists.txt`
- 验证：`tests/CMakeLists.txt`

- [ ] **步骤 1：重新配置并执行 MSVC Release 全量构建**

运行：

```powershell
cmd.exe /c "call D:\study\VS\community\VC\Auxiliary\Build\vcvars64.bat && D:\study\qt\Tools\CMake_64\bin\cmake.exe -S . -B build-codex-msvc-release -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release && D:\study\qt\Tools\CMake_64\bin\cmake.exe --build build-codex-msvc-release --config Release"
```

预期：配置与构建成功。

- [ ] **步骤 2：运行完整测试套件**

运行：

```powershell
$env:Path='D:\study\qt\6.8.3\msvc2022_64\bin;' + $env:Path
$env:QT_PLUGIN_PATH='D:\study\qt\6.8.3\msvc2022_64\plugins'
$env:QT_QPA_PLATFORM='offscreen'
D:\study\qt\Tools\CMake_64\bin\ctest.exe --test-dir build-codex-msvc-release -C Release --output-on-failure
```

预期：全部 16 项测试通过。

- [ ] **步骤 3：检查提交边界**

运行：

```powershell
git diff --check
git status --short
git log -3 --oneline
```

预期：修复文件已提交；用户原有未提交文件仍保持未暂存，且没有被本次修复覆盖。
