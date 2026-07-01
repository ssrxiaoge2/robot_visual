# Main Window UI Layout and Theme Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将主窗口调整为“左侧独立整列、右侧上方流程监控、右侧下方日志”的布局，并修复 FIFO 隔行配色及增强左侧面板标题辨识度。

**Architecture:** 继续由 `MainWindow::initUI()` 动态创建界面，不修改 `.ui` 文件、业务对象、信号连接或数据刷新逻辑。左侧现有控件树整体放入带主题外观的独立 `QWidget`，内部保持竖向滚动；右侧用 `QVBoxLayout` 组合现有 `WorkflowWidget` 与日志控件。深浅色、表格隔行色和标题层级统一由 `MainWindow::applyTheme()` 的全局 QSS 管理。

**Tech Stack:** C++17、Qt 6 Widgets、Qt Style Sheets（QSS）、CMake

---

## 文件结构与范围

| 文件 | 操作 | 职责 |
|---|---|---|
| `src/mainwindow.cpp` | 修改 | 主窗口布局、左侧整体容器、面板标题属性、深浅主题与 FIFO 隔行色 |

不新增程序文件，不修改 `src/mainwindow.h`、`src/mainwindow.ui`、设备控制、调度、日志写入、信号槽或队列刷新逻辑。计划文档本身不计入程序文件。

### Task 1: 建立 UI 结构与样式的失败基线检查

**Files:**
- Inspect: `src/mainwindow.cpp:450-559`
- Inspect: `src/mainwindow.cpp:624-659`
- Inspect: `src/mainwindow.cpp:1294-1351`

- [ ] **Step 1: 运行布局结构检查并确认当前失败**

Run:

```bash
python3 - <<'PY'
from pathlib import Path

source = Path("src/mainwindow.cpp").read_text(encoding="utf-8")
checks = {
    "left column wrapper": 'setObjectName("leftColumn")' in source,
    "right vertical layout": "auto *rightArea = new QVBoxLayout" in source,
    "log belongs to right layout": "rightArea->addWidget(m_logView" in source,
    "dark alternating row color": "alternate-background-color:#2d3036" in source,
    "light alternating row color": "alternate-background-color:#f3f6f9" in source,
    "emphasized group titles": "font-weight:600" in source,
}
failed = [name for name, passed in checks.items() if not passed]
assert not failed, "missing UI changes: " + ", ".join(failed)
PY
```

Expected: FAIL with `AssertionError: missing UI changes:` listing the absent layout and theme markers.

- [ ] **Step 2: 记录现有业务接线的保护基线**

Run:

```bash
python3 - <<'PY'
from pathlib import Path

source = Path("src/mainwindow.cpp").read_text(encoding="utf-8")
required = [
    "connect(m_btnStart",
    "connect(m_btnStop",
    "connect(m_btnReset",
    "connect(m_lineStartBtn",
    "connect(m_lineStopBtn",
    "connect(m_lineResetBtn",
    "void MainWindow::updateLineQueue",
]
missing = [marker for marker in required if marker not in source]
assert not missing, "baseline missing: " + ", ".join(missing)
print("UI business-wiring baseline: PASS")
PY
```

Expected: `UI business-wiring baseline: PASS`。

### Task 2: 重排主窗口为左整列、右上下分区

**Files:**
- Modify: `src/mainwindow.cpp:450-559`

- [ ] **Step 1: 更新 `initUI()` 上方的布局说明**

将布局注释替换为：

```cpp
 * 布局结构：
 *   VBoxLayout (root)
 *     ├── HBoxLayout (toolbar)     工具栏
 *     └── HBoxLayout (mainArea)
 *           ├── QWidget (leftColumn)  左侧独立整列
 *           │     └── QScrollArea     设备状态、调度、配置和调试面板
 *           └── VBoxLayout (rightArea)
 *                 ├── WorkflowWidget  上方流程监控
 *                 └── QPlainTextEdit  下方日志区
```

- [ ] **Step 2: 给左侧内容留出整体容器内边距**

在创建 `leftPanel` 后，把：

```cpp
leftPanel->setContentsMargins(0, 0, 4, 0);
```

替换为：

```cpp
leftPanel->setContentsMargins(8, 8, 8, 8);
leftPanel->setSpacing(8);
```

这只改变控件排布，不改变任何子面板的创建顺序和行为。

- [ ] **Step 3: 用独立 `QWidget` 包住左侧滚动区**

将当前 `scrollArea` 创建和 `mainArea->addWidget(scrollArea)` 代码替换为：

```cpp
    // 左侧整列是一个独立视觉区域；内容超出高度时只在本列上下滚动。
    auto *leftColumn = new QWidget();
    leftColumn->setObjectName("leftColumn");
    leftColumn->setMinimumWidth(300);
    leftColumn->setMaximumWidth(350);

    auto *leftColumnLayout = new QVBoxLayout(leftColumn);
    leftColumnLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidget(leftContainer);
    leftColumnLayout->addWidget(scrollArea);
    mainArea->addWidget(leftColumn);
```

保留 `leftContainer->setMinimumWidth(280)`。明确关闭横向滚动条，避免窄屏时出现左右拖动；竖向滚动条仍按内容高度自动出现。

- [ ] **Step 4: 将流程监控与日志放入右侧纵向布局**

将原先流程图加入 `mainArea`、`root` 加入日志的代码整体替换为：

```cpp
    // 右侧上方显示流程监控，下方显示日志，两者只占右侧区域。
    auto *rightArea = new QVBoxLayout();
    rightArea->setContentsMargins(0, 0, 0, 0);
    rightArea->setSpacing(8);

    m_flow = new WorkflowWidget();
    m_flow->setMinimumSize(700, 280);
    rightArea->addWidget(m_flow, 2);

    m_logView = new QPlainTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(200);
    m_logView->setMinimumHeight(180);
    rightArea->addWidget(m_logView, 1);

    mainArea->addLayout(rightArea, 1);
    root->addLayout(mainArea, 1);
```

`2:1` 是默认高度比例：流程监控优先获得空间，日志仍保持可用高度。不要移动或改写后续任何 `connect(...)`。

- [ ] **Step 5: 运行结构检查**

Run:

```bash
python3 - <<'PY'
from pathlib import Path

source = Path("src/mainwindow.cpp").read_text(encoding="utf-8")
assert 'setObjectName("leftColumn")' in source
assert "auto *rightArea = new QVBoxLayout" in source
assert "rightArea->addWidget(m_flow, 2)" in source
assert "rightArea->addWidget(m_logView, 1)" in source
assert "mainArea->addLayout(rightArea, 1)" in source
assert "root->addWidget(m_logView" not in source
assert "Qt::ScrollBarAlwaysOff" in source
print("main-window layout structure: PASS")
PY
```

Expected: `main-window layout structure: PASS`。

- [ ] **Step 6: 提交布局变更**

```bash
git add src/mainwindow.cpp
git commit -m "ui: reorganize main window into two columns"
```

### Task 3: 修复 FIFO 隔行色并增强左侧卡片标题层级

**Files:**
- Modify: `src/mainwindow.cpp:624-649`
- Modify: `src/mainwindow.cpp:1294-1351`

- [ ] **Step 1: 标记调度监控内部的小节标题**

创建“模拟缺料”和“FIFO 队列”标签后分别设置统一属性：

```cpp
    auto *stationTitle = new QLabel(QStringLiteral("模拟缺料"));
    stationTitle->setProperty("sectionTitle", true);
    layout->addWidget(stationTitle);
```

```cpp
    auto *queueTitle = new QLabel(QStringLiteral("FIFO 队列"));
    queueTitle->setProperty("sectionTitle", true);
    layout->addWidget(queueTitle);
```

不要给普通数据标签加粗，避免状态值与栏目标题争夺视觉注意力。

- [ ] **Step 2: 增强深色主题的左侧容器和标题签层级**

在 `kDark` 中保留现有基础色，加入或替换相应规则，使相关片段为：

```cpp
        "QWidget#leftColumn { background:#242629; border:1px solid #444; border-radius:7px; }"
        "QWidget#leftColumn QScrollArea { background:transparent; border:none; }"
        "QGroupBox          { color:#ddd; border:1px solid #444; border-radius:6px;"
        "                     margin-top:16px; padding-top:14px; background:#242629; }"
        "QGroupBox::title   { subcontrol-origin:margin; left:12px; padding:2px 10px 3px 10px;"
        "                     color:#f7fbff; font-size:13px; font-weight:600; background:#2f5f85;"
        "                     border:1px solid #4f7da2; border-radius:4px; }"
        "QLabel[sectionTitle=\"true\"] { color:#f0f2f5; font-size:13px; font-weight:600;"
        "                                  padding-top:4px; }"
```

将深色表格规则改为：

```cpp
        "QTableWidget { background:#34373e; alternate-background-color:#2d3036;"
        "               color:#ddd; gridline-color:#555; border:1px solid #555; font-size:12px; }"
```

左侧面板标题采用贴合边框的蓝灰色“标题签”样式，不再是只有文字或整条横栏；隔行色采用同一深色主题中的两个近邻灰色，不再使用系统默认白色。

- [ ] **Step 3: 为浅色主题提供对应标题签层级与隔行色**

在 `kLight` 中加入或替换对应规则：

```cpp
        "QWidget#leftColumn { background:#ffffff; border:1px solid #ccc; border-radius:7px; }"
        "QWidget#leftColumn QScrollArea { background:transparent; border:none; }"
        "QGroupBox          { color:#333; border:1px solid #ccc; border-radius:6px;"
        "                     margin-top:16px; padding-top:14px; background:#ffffff; }"
        "QGroupBox::title   { subcontrol-origin:margin; left:12px; padding:2px 10px 3px 10px;"
        "                     color:#f7fbff; font-size:13px; font-weight:600; background:#3f7fb1;"
        "                     border:1px solid #5b94c1; border-radius:4px; }"
        "QLabel[sectionTitle=\"true\"] { color:#1f2933; font-size:13px; font-weight:600;"
        "                                  padding-top:4px; }"
```

将浅色表格规则改为：

```cpp
        "QTableWidget { background:#ffffff; alternate-background-color:#f3f6f9;"
        "               color:#222; gridline-color:#ccc; border:1px solid #bbb; font-size:12px; }"
```

浅色主题保留白色基行，但交替行使用浅蓝灰，文字继续使用深色；左侧面板标题统一用蓝色标题签，和你确认的参考图风格保持一致。

- [ ] **Step 4: 运行主题静态检查**

Run:

```bash
python3 - <<'PY'
from pathlib import Path

source = Path("src/mainwindow.cpp").read_text(encoding="utf-8")
required = [
    'stationTitle->setProperty("sectionTitle", true)',
    'queueTitle->setProperty("sectionTitle", true)',
    'QWidget#leftColumn',
    'QLabel[sectionTitle=\\"true\\"]',
    'alternate-background-color:#2d3036',
    'alternate-background-color:#f3f6f9',
    'background:#2f5f85',
    'background:#3f7fb1',
]
missing = [marker for marker in required if marker not in source]
assert not missing, "missing theme rules: " + ", ".join(missing)
assert source.count("font-weight:600") >= 4
assert "m_lineQueueTable->setAlternatingRowColors(true)" in source
print("theme and FIFO readability rules: PASS")
PY
```

Expected: `theme and FIFO readability rules: PASS`。

- [ ] **Step 5: 提交主题变更**

```bash
git add src/mainwindow.cpp
git commit -m "ui: improve panel hierarchy and FIFO row contrast"
```

### Task 4: 构建与人工验收

**Files:**
- Verify: `src/mainwindow.cpp`

- [ ] **Step 1: 确认变更范围只有 UI 代码**

Run:

```bash
git diff HEAD~2 -- src/mainwindow.cpp
git diff HEAD~2 --name-only
```

Expected: 文件列表只有 `src/mainwindow.cpp`；差异只涉及 `initUI()`、两个小节标题属性和 `applyTheme()` QSS，不包含槽函数、业务信号、`updateLineQueue()` 或设备逻辑变更。

- [ ] **Step 2: 重新运行全部现有脚本测试**

Run:

```bash
for test_file in scripts/test_*.py; do python3 "$test_file" || exit 1; done
```

Expected: 所有脚本退出码为 `0`，无 `AssertionError`。

- [ ] **Step 3: 配置并构建 Qt 程序**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
```

Expected: CMake 配置成功，`wh-robot-visual` 目标构建完成，无编译或链接错误。

- [ ] **Step 4: 启动程序执行深色主题人工验收**

Run:

```bash
./build/wh-robot-visual
```

Expected:

- 顶部工具栏横跨窗口且行为不变。
- 左侧是从主体顶部延伸到底部的独立整列，内部可上下滚动，无横向滚动条。
- 右侧流程监控在上、日志在下，日志左边缘不再延伸到左侧列下方。
- 左侧每个 `QGroupBox` 标题比正文更亮、更粗，且没有被边框遮挡。
- “模拟缺料”和“FIFO 队列”标题醒目，但状态值和普通字段没有整体加粗。
- FIFO 相邻行使用两种深灰背景，所有单元格文字清晰可读。

- [ ] **Step 5: 切换浅色主题并检查窗口缩放**

Expected:

- 浅色 FIFO 基行是白色、交替行是浅蓝灰，文字保持深色。
- 左侧整体容器、各子面板、流程区和日志区颜色属于同一浅色主题，无突兀的深色残留。
- 在窗口最小尺寸 `1100×750` 和默认尺寸 `1200×850` 下，右侧流程图与日志均可用。
- 缩小窗口高度后，左列出现竖向滚动条；右侧日志仍留有至少 `180px` 高度。
- 切换主题两次后，颜色和标题样式能即时恢复，不发生样式粘滞。

- [ ] **Step 6: 检查工作区并提交验收修正（仅在确有修正时）**

```bash
git status --short
git diff --check
```

Expected: `git diff --check` 无输出。若人工验收产生必要的纯 UI 修正：

```bash
git add src/mainwindow.cpp
git commit -m "ui: polish main window layout validation"
```

若无需修正，不创建空提交。

## 自检结果

- 需求覆盖：FIFO 隔行可读性、左侧独占整列、右侧流程/日志上下排列、左侧整体 Widget、面板标题签层级、深浅主题一致性均有对应任务。
- 范围约束：实施只修改 `src/mainwindow.cpp`，没有新增程序文件，也不修改业务逻辑。
- 一致性：计划统一使用 `leftColumn`、`rightArea`、`sectionTitle` 三个标识；深浅主题规则成对出现。
- 无占位内容：所有代码、命令、预期结果和人工验收项均已明确。
