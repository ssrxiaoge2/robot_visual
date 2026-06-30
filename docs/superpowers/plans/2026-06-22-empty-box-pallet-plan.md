# 空箱码垛配置与点位规划 实现计划

> **面向 AI 代理的工作者：** 按任务顺序实现并使用复选框（`- [x]`）跟踪进度。每完成一个阶段先编译验证，再进入下一阶段。除本计划列出的文件外，不要顺手重构其他模块。

**目标：** 新增独立的空箱码垛配置、点位计算、状态缓存、视觉 HTTP 占用检测与调试 UI。现场存在大箱码垛区和小箱码垛区，两套参数必须独立保存。主流程未来进入“放置空箱码垛”步骤时，优先通过统一接口获取相对机械臂初始点位的偏移量；机械臂运动到初始点位叠加偏移后松开夹爪，未来由主流程确认放置完成并提交已放数量。本期实现模块、配置 UI、视觉检测接口和主流程接入预留，不对接机械臂执行结果，不把码垛动作接入现有整线流程。

**架构：** `PalletScheduler` 是纯码垛规划与状态模块，负责配置、校验、容量计算、下一相对偏移/绝对预览、仿真、已放数量缓存和视觉空区防抖清零；它不控制机械臂、不直接发 HTTP、不判断箱型来源。`VisionHttpClient` 增加独立的码垛区占用检测请求，复用现有 `/inference` HTTP 结果，但不触发现有抓取坐标信号，避免影响取料闭环。`PalletParamDialog` 是独立 Qt Widgets 配置窗口，负责两套参数编辑、校验结果、下一点预览、8 层仿真、人工数量修正、视觉检测和清零。`MainWindow` 只新增一个“码垛配置”入口按钮，并把 `PalletScheduler` 与 `VisionHttpClient` 传给配置窗口。后续主流程、扫码结果或客户系统必须显式传入 `PalletArea`。

**技术栈：** Qt 6（Widgets/Core/Network）、CMake、QSettings。沿用当前工程动态构建 UI 的方式，不新增 `.ui` 文件。

**关键约束：**

- 允许修改 `MainWindow` 作为 UI 入口，修改 `CMakeLists.txt` 作为构建接入，修改 `VisionHttpClient` 增加独立占用检测接口；不修改 `huayanScheduler`、`lineorchestrator`、`devicemanager` 的现有流程逻辑。
- `PalletScheduler::nextPose()` 只计算下一点，不推进状态；真实主流程必须等机械臂确认“到点并松爪完成”后才调用 `commitPlaced()`。配置窗口的“模拟放置完成”按钮仅用于仿真验证，用它代替机械臂完成信号推进缓存。
- `originPose` 定义为机械臂/现场示教的码垛初始点位，仅用于绝对点位预览；主流程优先使用 `nextRelativeOffset()` 输出的相对偏移。
- 默认每层从 `负X、负Y` 角开始，先沿 X 放满，再换 Y 行；通过 `invertX/invertY` 适配现场坐标方向。
- 最大层数默认 8，UI 和算法均限制 1-8。
- 相对偏移的 Z 为 `palletSize.z + layer * boxSize.z`；托盘高度必须并入总高度。默认现场值托盘高为 0 时第一层 Z 偏移仍为 0；若现场填入真实托盘高度，则第一层 Z 偏移等于托盘高度。`releaseZOffset` 作为现场推荐/记录参数，不在最终 Z 偏移中重复叠加。绝对预览才使用 `originPose + offset`。
- 视觉连续 3 次识别为空才允许自动清零，并且必须满足“当前不在码垛执行中、当前已放数量 > 0”。视觉请求失败或识别不确定不修改缓存。
- UI 深色/浅色主题必须跟随 `MainWindow::applyTheme(bool dark)` 的全局 QSS，不写只适配深色的固定样式。
- 新增代码需要中文注释说明坐标基准、状态提交语义、视觉 HTTP 结果语义、视觉清零防抖和现场调试注意事项。

---

## 现场模拟参数

本计划内置一组现场模拟参数，供“推荐配置/填入默认现场值”和算法验收使用，单位统一为 mm。

| 参数 | 值 |
|------|----|
| 托盘/码垛区域 | `1100 x 1100 x 0`，来自 110cm x 110cm；高度未给定，v1 记为 0 |
| 小框 | `450 x 270 x 108`，来自 45cm x 27cm x 10.8cm |
| 大框 | `580 x 380 x 288`，来自 58cm x 38cm x 28.8cm |
| 默认箱间距 | `20` |
| 默认边缘预留 | `20` |
| 默认最大层数 | `8` |
| 默认释放高度参考值 | `1.2 * boxSize.z`；小框 `129.6`，大框 `345.6`，仅用于现场确定机械臂初始释放高度，不参与偏移公式 |

按默认间距/边缘预留计算：

| 区域 | 列数 X | 行数 Y | 单层容量 | 8 层容量 |
|------|--------|--------|----------|----------|
| 小框 | 2 | 3 | 6 | 48 |
| 大框 | 1 | 2 | 2 | 16 |

说明：v1 不做箱体旋转优化，直接使用配置的 `boxSize.x/y`。以上容量用于测试预期；若现场允许旋转箱体，另起 v2 设计。

---

## 文件清单

### 新增文件

| 文件 | 职责 |
|------|------|
| `src/palletscheduler.h` | 声明码垛区域、尺寸、点位、配置、仿真项和 `PalletScheduler` 接口 |
| `src/palletscheduler.cpp` | 实现容量计算、点位计算、参数校验、状态缓存、视觉空区防抖和仿真 |
| `src/palletparamdialog.h` | 声明码垛配置对话框、控件集合、视觉检测槽和 UI 刷新槽 |
| `src/palletparamdialog.cpp` | 实现大箱/小箱配置页、按钮行为、视觉检测结果展示、表格仿真和主题兼容；整数输入使用 `QLineEdit + QIntValidator`，小数 SpinBox 屏蔽鼠标滚轮 |

### 修改文件

| 文件 | 职责变化 |
|------|----------|
| `CMakeLists.txt` | 加入 `palletscheduler` 和 `palletparamdialog` 源文件 |
| `src/visionclient.h` | 增加独立码垛区占用检测接口和结果信号 |
| `src/visionclient.cpp` | 复用 `/inference` HTTP 结果解析占用状态，不触发现有抓取坐标信号 |
| `src/mainwindow.h` | 声明码垛配置按钮、Scheduler 成员和打开配置窗口槽 |
| `src/mainwindow.cpp` | 新增“码垛配置”入口面板，创建/传递 `PalletScheduler` 与 `VisionHttpClient`，打开 `PalletParamDialog` |

### 不修改文件

- `src/mainwindow.ui`：当前主界面由 `MainWindow::initUI()` 动态构建，新入口沿用该模式。
- `src/huayanScheduler.*`、`src/lineorchestrator.*`、`src/devicemanager.*`：本期不接主流程，不改变现有执行时序。

---

## 阶段总览

| 阶段 | 目标 | 独立验证出口 |
|------|------|--------------|
| 阶段一 | 实现 `PalletScheduler` 数据结构、算法和校验 | 可用临时调用或 UI 前测试验证容量、点位、满载和非法配置 |
| 阶段二 | 实现 QSettings 配置/状态缓存和视觉空区防抖状态机 | 重启后配置/已放数量恢复，连续空识别逻辑可验证 |
| 阶段三 | `VisionHttpClient` 增加码垛占用 HTTP 检测 | `/inference` object_count 能转换为 occupied/empty，不影响取料信号 |
| 阶段四 | 实现 `PalletParamDialog` 配置 UI | 两套配置可编辑、校验、预览、仿真、视觉检测、清零、人工修正 |
| 阶段五 | 接入 `MainWindow` 和 CMake | 主窗口出现入口，点击打开配置窗口，现有功能不变 |
| 阶段六 | 编译、手动验证和现场 dry-run 清单 | CMake/build 成功，UI、视觉检测和算法验收清单通过；不验证机械臂真实放置完成 |

---

## 阶段一：实现 PalletScheduler 纯算法

**文件：**
- 创建：`src/palletscheduler.h`
- 创建：`src/palletscheduler.cpp`
- 修改：`CMakeLists.txt`

- [x] **步骤 1.1：声明基础类型和配置结构**

在 `palletscheduler.h` 中声明：

```cpp
enum class PalletArea {
    LargeBox,
    SmallBox
};

struct PalletPose {
    double x = 0;
    double y = 0;
    double z = 0;
    double rx = 0;
    double ry = 0;
    double rz = 0;
};

struct PalletSize {
    double x = 0;
    double y = 0;
    double z = 0;
};

struct PalletConfig {
    PalletSize palletSize;
    PalletSize boxSize;
    double gapX = 20.0;
    double gapY = 20.0;
    double marginX = 20.0;
    double marginY = 20.0;
    int maxLayers = 8;
    double releaseZOffset = 0.0;
    double maxRobotZ = 0.0; // 0 表示不校验最高安全 Z
    bool invertX = false;
    bool invertY = false;
    PalletPose originPose;
};
```

`originPose` 是机械臂初始点位，主要用于绝对预览；真正送给机械臂的值优先使用 `nextRelativeOffset()`。若机械臂先运动到该初始点位，则第 1 层相对 Z 偏移为 `palletSize.z`，第 N 层相对 Z 偏移为 `palletSize.z + (N-1) * boxSize.z`。默认现场值 `palletSize.z=0` 时第一层仍为 0。`releaseZOffset` 保留为 UI 推荐和现场记录值，避免实现时重复上抬 Z。

同时声明仿真输出结构：

```cpp
struct PalletSimulationItem {
    int index = 0;
    int layer = 0;
    int row = 0;
    int column = 0;
    PalletPose offset;
    PalletPose pose;
};
```

- [x] **步骤 1.2：声明 `PalletScheduler` 接口**

`PalletScheduler` 继承 `QObject`，提供配置、校验、容量、下一点、提交、清零、人工修正和仿真接口：

```cpp
class PalletScheduler : public QObject {
    Q_OBJECT
public:
    explicit PalletScheduler(QObject *parent = nullptr);

    void setConfig(PalletArea area, const PalletConfig &config);
    PalletConfig config(PalletArea area) const;

    bool validateConfig(PalletArea area, QStringList *errors, QStringList *suggestions) const;
    int columns(PalletArea area) const;
    int rows(PalletArea area) const;
    int perLayerCapacity(PalletArea area) const;
    int totalCapacity(PalletArea area) const;

    bool nextPose(PalletArea area, PalletPose *pose, QString *error) const;
    bool nextRelativeOffset(PalletArea area, PalletPose *offset, QString *error) const;
    bool commitPlaced(PalletArea area, QString *error);
    void reset(PalletArea area);
    bool setPlacedCount(PalletArea area, int count, QString *error);
    int placedCount(PalletArea area) const;

    QList<PalletSimulationItem> simulate(PalletArea area, int maxCount, QString *error) const;

    void setStackingActive(PalletArea area, bool active);
    void markAreaObservedEmpty(PalletArea area);
    void markAreaObservedOccupied(PalletArea area);
    int emptyObserveCount(PalletArea area) const;
    QDateTime lastAutoResetTime(PalletArea area) const;

signals:
    void areaFull(PalletArea area);
    void areaAutoReset(PalletArea area, const QString &reason);
    void stateChanged(PalletArea area);
};
```

- [x] **步骤 1.3：实现容量计算**

计算规则：

```text
usableX = palletSize.x - 2 * marginX
usableY = palletSize.y - 2 * marginY
columns = floor((usableX + gapX) / (boxSize.x + gapX))
rows    = floor((usableY + gapY) / (boxSize.y + gapY))
perLayerCapacity = columns * rows
totalCapacity = perLayerCapacity * maxLayers
```

非法配置时容量接口返回 0，避免除零。`maxLayers` 在计算时限制到 1-8，但校验仍报告原始非法值。

`palletSize.z` 允许为 0；只校验 `palletSize.x/y > 0`。`boxSize.x/y/z` 必须大于 0。

- [x] **步骤 1.4：实现下一点位计算**

下一箱索引：

```text
indexInLayer = placedCount % perLayerCapacity
layer        = placedCount / perLayerCapacity
column       = indexInLayer % columns
row          = indexInLayer / columns
```

偏移计算：

```text
totalUsedX = columns * boxSize.x + (columns - 1) * gapX
totalUsedY = rows * boxSize.y + (rows - 1) * gapY
startX = -totalUsedX / 2 + boxSize.x / 2
startY = -totalUsedY / 2 + boxSize.y / 2
offsetX = startX + column * (boxSize.x + gapX)
offsetY = startY + row * (boxSize.y + gapY)
offsetZ = palletSize.z + layer * boxSize.z
```

方向修正：

```text
if invertX: offsetX = -offsetX
if invertY: offsetY = -offsetY
```

绝对点位：

```text
pose.x = originPose.x + offsetX
pose.y = originPose.y + offsetY
pose.z = originPose.z + offsetZ  // 仅绝对预览；offsetZ 已包含托盘高度
pose.rx/ry/rz = originPose.rx/ry/rz
```

若 `placedCount >= totalCapacity`，`nextPose()` 返回 false，错误为“码垛区已满，请搬运”，并发出 `areaFull(area)`。

- [x] **步骤 1.5：实现参数校验和推荐提示**

`validateConfig()` 必须填充错误和建议：

- `palletSize.x/y > 0`，`palletSize.z >= 0`。
- `boxSize.x/y/z > 0`。
- 间距、边缘预留不小于 0。
- 最大层数范围 1-8。
- `releaseZOffset >= 0`，作为推荐/记录字段；可用 `boxSize.z * 1.0` 到 `boxSize.z * 1.5` 辅助现场确定机械臂第一层安全释放高度，但主流程输出仍以相对偏移为准。
- 扣除边距后的可用区域至少能放下一个箱子。
- 当前已放数量不能超过总容量。
- 箱子外沿不能超出扣除边距后的可用区域。
- 若 `maxRobotZ > 0` 且使用绝对预览/绝对点位，最高层 `originPose.z + palletSize.z + (maxLayers - 1) * boxSize.z` 不能超过 `maxRobotZ`。

- [x] **步骤 1.6：实现现场模拟默认值工具**

提供静态工具或 UI 可复用函数生成现场默认配置：

```cpp
static PalletConfig defaultLargeBoxConfig();
static PalletConfig defaultSmallBoxConfig();
```

默认值：托盘 `1100 x 1100 x 0`；小框 `450 x 270 x 108`；大框 `580 x 380 x 288`；gap/margin `20`；maxLayers `8`；releaseZOffset 记录值 `boxSize.z * 1.2`。

- [x] **步骤 1.7：实现仿真**

`simulate(area, maxCount)` 从当前配置的第 0 个箱子开始生成点位，不改变真实 `placedCount`。`maxCount` 传入 0 或负数时默认生成 `totalCapacity`，但 UI 的“仿真 8 层”最多生成 `totalCapacity`。达到容量上限后停止，不生成无效点。

- [x] **阶段一验证**

通过临时调用或最小测试验证：

1. 小框现场默认配置得到 `2 x 3 = 6/层`，8 层总容量 48。
2. 大框现场默认配置得到 `1 x 2 = 2/层`，8 层总容量 16。
3. 第 1 箱位于 `负X、负Y` 角。
4. 一层放满后进入下一层。
5. `invertX/invertY` 能翻转点位方向。
6. 第 8 层放满后 `nextPose()` 返回满载错误。
7. `maxRobotZ` 能拦截过高点位。

---

## 阶段二：实现状态缓存和视觉空区防抖状态机

**文件：**
- 修改：`src/palletscheduler.h`
- 修改：`src/palletscheduler.cpp`

- [x] **步骤 2.1：定义 QSettings 键**

沿用现有设置组织和应用名风格：

```cpp
static const char *kSettingsOrg = "wh-robot";
static const char *kSettingsApp = "robot-visual";
```

建议键前缀：

```text
pallet/large/config/*
pallet/small/config/*
pallet/large/placedCount
pallet/small/placedCount
pallet/large/emptyObserveCount
pallet/small/emptyObserveCount
pallet/large/lastAutoResetTime
pallet/small/lastAutoResetTime
```

- [x] **步骤 2.2：构造时加载默认配置和缓存状态**

`PalletScheduler` 构造时加载两套配置和状态。没有配置时使用现场模拟默认值：托盘 1100x1100x0，小框 450x270x108，大框 580x380x288，间距/边距 20，最大层数 8，释放高度记录值 `1.2 * boxSize.z`；主流程输出相对偏移，其中 Z 已包含托盘高度，绝对预览才叠加现场示教的 `originPose`。

- [x] **步骤 2.3：保存配置和状态**

`setConfig()`、`commitPlaced()`、`setPlacedCount()`、`reset()` 成功后立即保存到 `QSettings` 并发出 `stateChanged(area)`。

`commitPlaced()` 只在当前未满载时把 `placedCount + 1`；如果已经满载，返回 false 并发出 `areaFull(area)`。

- [x] **步骤 2.4：实现人工修正已放数量**

`setPlacedCount()` 只允许 `0 <= count <= totalCapacity`。非法时返回 false 和错误信息，不改状态。

- [x] **步骤 2.5：实现视觉空区防抖接口**

`markAreaObservedEmpty()` 行为：

- 若 `placedCount == 0`，只清空连续空计数。
- 若该区域 `stackingActive == true`，直接忽略本次空识别，避免机械臂执行中误清零。
- 连续空识别计数达到 3 时调用 `reset(area)`，记录 `lastAutoResetTime`，发出 `areaAutoReset(area, reason)`。

`markAreaObservedOccupied()` 行为：清空连续空计数，不修改 `placedCount`。

- [x] **阶段二验证**

1. 修改配置后重新创建 `PalletScheduler` 能恢复配置。
2. `commitPlaced()` 后重新创建对象能恢复已放数量。
3. `setPlacedCount()` 修正数量后下一点按新数量计算。
4. 连续 3 次空识别且非执行中会自动清零。
5. 执行中空识别不会自动清零。
6. 识别到有箱会清空连续空计数。

---

## 阶段三：VisionHttpClient 增加码垛区占用检测

**文件：**
- 修改：`src/visionclient.h`
- 修改：`src/visionclient.cpp`

- [x] **步骤 3.1：声明独立 HTTP 检测接口**

在 `VisionHttpClient` 中新增 public slot：

```cpp
void fetchPalletOccupancy(const QString &requestId);
```

新增信号：

```cpp
void palletOccupancyReady(QString requestId,
                          bool occupied,
                          int objectCount,
                          double bestConfidence,
                          QString summary);
void palletOccupancyError(QString requestId, QString msg);
```

`requestId` 由调用方传入，例如 `large`、`small` 或带时间戳的唯一值，方便 UI 把异步结果映射回当前 tab。

- [x] **步骤 3.2：复用 `/inference` 但隔离现有抓取流程**

`fetchPalletOccupancy()` 请求同一个 `GET /inference`，但必须独立解析 reply，不调用 `parseInferenceReply()`，不 emit `rawCoordinatesReady`、`coordinatesReady`、`noObjectDetected`、`errorOccurred`，避免影响现有取料闭环和 `HuayanScheduler::setGrabOffset`。

解析规则：

```text
object_count > 0 -> occupied = true
object_count == 0 或 objects 为空 -> occupied = false
网络/JSON 错误 -> palletOccupancyError，不修改 PalletScheduler 状态
bestConfidence = objects 中 confidence 最大值；没有则 0
summary = 简短 JSON 摘要，用于 UI 日志/提示
```

- [x] **步骤 3.3：明确视觉检测前置条件**

文档和代码注释必须说明：该接口只判断当前相机视野内是否有目标。调用前必须保证 AGV/相机位姿已经对准对应码垛区；否则 `occupied=false` 不代表真实码垛区为空。

- [x] **阶段三验证**

1. 未配置视觉 IP 时返回 `palletOccupancyError`。
2. `/inference` 返回 `object_count=0` 时得到 `occupied=false`。
3. `/inference` 返回 `object_count>0` 时得到 `occupied=true` 和最大 confidence。
4. 调用占用检测不会触发现有 `rawCoordinatesReady` 或机械臂抓取偏移。
5. 网络/JSON 错误不会修改 `PalletScheduler` 的已放数量。

---

## 阶段四：实现 PalletParamDialog 配置 UI

**文件：**
- 创建：`src/palletparamdialog.h`
- 创建：`src/palletparamdialog.cpp`
- 修改：`CMakeLists.txt`

- [x] **步骤 4.1：创建对话框基础结构**

`PalletParamDialog` 继承 `QDialog`，构造函数接收 `PalletScheduler *scheduler` 和 `VisionHttpClient *visionClient`。对话框不拥有这两个对象，只保存指针。设置窗口标题“空箱码垛配置”，建议初始尺寸 `980x720`，最小尺寸 `860x620`。

根布局：

```text
QVBoxLayout root
  QTabWidget tabs
    tab: 大箱码垛
    tab: 小箱码垛
  bottom button row: 关闭
```

每个 tab 使用同一个构建函数创建，避免两套 UI 代码散开。

- [x] **步骤 4.2：每个配置页布局**

每个 tab 使用左右分栏：

```text
QHBoxLayout page
  左侧：参数区（QScrollArea + QVBoxLayout）
  右侧：结果区（QVBoxLayout）
```

左侧参数区按 `QGroupBox` 分组：

| 分组 | 控件 |
|------|------|
| 码垛区域尺寸 | `palletSize.x/y/z`，屏蔽滚轮的小数输入，单位 mm |
| 空箱尺寸 | `boxSize.x/y/z`，屏蔽滚轮的小数输入，单位 mm |
| 间距与边缘 | `gapX/gapY/marginX/marginY`，屏蔽滚轮的小数输入，单位 mm |
| 层数与高度 | `maxLayers` 使用 `QLineEdit + QIntValidator(1-8)`，`releaseZOffset` 和 `maxRobotZ` 使用屏蔽滚轮的小数输入 |
| 中心示教点位 | `originPose.x/y/z/rx/ry/rz`，屏蔽滚轮的小数输入；仅用于绝对预览 |
| 方向修正 | `invertX`、`invertY`，QCheckBox |
| 状态修正 | 当前已放数量 `QLineEdit + QIntValidator` + “应用数量修正”按钮 |
| 视觉检测 | “检测当前区域”按钮 + 最近结果 QLabel + 连续空计数 QLabel |

控件范围建议：整数项使用 `QLineEdit + QIntValidator`，避免鼠标滚轮误改；小数尺寸/坐标使用屏蔽滚轮的 `QDoubleSpinBox`。尺寸和距离 `0-100000`，精度 1 位或 2 位；姿态角 `-360~360`；坐标 `-100000~100000`。`palletSize.z` 允许 0。

- [x] **步骤 4.3：每个配置页按钮区**

每页按钮固定为：

```text
[保存配置] [校验配置] [填入默认现场值] [推荐释放高度] [计算下一点] [模拟放置完成] [仿真 8 层] [清除显示结果] [清零当前区域]
```

按钮行为：

- 保存配置：从控件读取 `PalletConfig`，调用 `setConfig()`，再刷新结果区。
- 校验配置：先读取当前控件值进行临时校验，不强制保存；显示 errors/suggestions。
- 填入默认现场值：当前 tab 为小箱时把输入框改为托盘 1100x1100x0、小框 450x270x108；当前 tab 为大箱时把输入框改为托盘 1100x1100x0、大框 580x380x288；gap/margin 20，maxLayers 8，releaseZOffset `1.2 * boxSize.z`。作用是快速恢复现场模拟基准参数，便于算法验收和现场初次配置；它不会读取当前 UI 值写入配置，也不会自动保存，只是把默认现场值写入输入框。
- 推荐释放高度：只更新 `releaseZOffset = boxSize.z * 1.2`，不改其他参数。
- 计算下一点：推荐要求先保存配置；显示下一箱层/行/列、相对偏移、绝对点位。该按钮只预览，不修改 `placedCount`，所以连续点击会显示同一个缓存状态下的下一点。
- 模拟放置完成：仅用于当前配置窗口仿真，调用 `commitPlaced(area)` 把已放数量推进 1，然后刷新下一点；真实主流程必须由机械臂完成回调触发提交。
- 仿真 8 层：从空托盘开始显示完整仿真表格，最多 `totalCapacity` 行；不修改真实 `placedCount`。
- 清除显示结果：清空右侧下一点/状态提示、校验结果和 8 层仿真表格；不修改配置、已放数量或视觉计数。
- 清零当前区域：弹出 `QMessageBox::question` 二次确认，确认后调用 `reset(area)`。
- 应用数量修正：弹出二次确认，调用 `setPlacedCount(area, value)`。
- 检测当前区域：调用 `VisionHttpClient::fetchPalletOccupancy(requestId)`，按钮进入“检测中”状态，收到结果后调用 `markAreaObservedOccupied/Empty`。

- [x] **步骤 4.4：视觉检测结果处理**

`PalletParamDialog` 连接 `VisionHttpClient::palletOccupancyReady/Error`：

- `occupied=true`：调用 `scheduler->markAreaObservedOccupied(area)`；UI 显示“检测到空箱/目标，继续沿用缓存”。
- `occupied=false`：调用 `scheduler->markAreaObservedEmpty(area)`；UI 显示连续空计数，例如 `1/3`、`2/3`、`已自动清零`。
- error：不调用 scheduler 状态修改接口；UI 显示错误，并保留原计数。
- 若 requestId 不属于当前窗口发起的检测，忽略，避免异步串台。

- [x] **步骤 4.5：结果区内容**

右侧结果区包含：

| 区域 | 控件 |
|------|------|
| 容量摘要 | QLabel 显示列数、行数、单层容量、总容量、当前已放 |
| 下一点位 | 只读 QLineEdit 或 QLabel，显示层/行/列、offsetX/Y/Z、x/y/z/rx/ry/rz |
| 视觉状态 | QLabel，显示最近 HTTP 检测结果、object_count、confidence、连续空计数 |
| 状态提示 | QLabel，显示正常、配置错误、已满、最近一次视觉自动清零时间 |
| 校验结果 | QPlainTextEdit，只读，显示错误和建议 |
| 仿真表格 | QTableWidget，列为序号/层/行/列/offsetX/offsetY/offsetZ/X/Y/Z/Rx/Ry/Rz |

表格要求：

- `horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents)`，最后一列 stretch。
- `verticalHeader()->setVisible(false)`。
- 数值统一保留 1 位或 2 位小数。
- 满载或配置错误时表格清空并显示错误。

- [x] **步骤 4.6：主题与风格**

不在对话框里写大段固定深色 QSS。优先依赖 `MainWindow::applyTheme()` 的全局 QWidget/QDialog/QGroupBox/QLineEdit/QTableWidget 样式。若确实需要状态色，只给状态 QLabel 设置少量动态颜色：正常绿色、警告黄色、错误红色，并保证浅色主题下可读。

界面风格沿用现有左侧面板：`QGroupBox`、`QFormLayout`、`QLineEdit/屏蔽滚轮的小数输入`、按钮高度 26-30px、边角 4-6px。

- [x] **步骤 4.7：UI 数据刷新规则**

每次以下事件后调用统一 `refreshPage(area)`：

- 打开窗口。
- 保存配置。
- 校验配置。
- 填入默认现场值。
- 推荐释放高度。
- 计算下一点。
- 模拟放置完成。
- 仿真。
- 清零。
- 应用数量修正。
- 视觉检测返回。
- 收到 `PalletScheduler::stateChanged(area)`。
- 收到 `areaFull/areaAutoReset`。

- [x] **阶段四验证**

1. 打开对话框后大箱/小箱两页均有完整参数控件。
2. “填入默认现场值”后，小框容量显示 48，大框容量显示 16。
3. 两页参数互不影响。
4. 保存后关闭重开能恢复。
5. 校验错误和建议清晰显示。
6. 下一点显示层/行/列、给机械臂的相对偏移和绝对预览坐标。
7. “模拟放置完成”后已放数量 +1，下一点随之推进；“计算下一点”本身不推进缓存。
8. 仿真表格列完整，满载后不生成无效点。
9. 清零和人工修正都有二次确认。
10. 视觉检测成功时更新占用/空区状态，失败时不修改缓存。
11. 深色/浅色主题下文字可读、控件不重叠。

### 仿真验证步骤

1. 打开主界面，进入“码垛配置”。
2. 在“小箱码垛”页点击“填入默认现场值”，确认托盘为 `1100 x 1100 x 0`，箱体为 `450 x 270 x 108`，间距/边距为 `20`，最大层数为 `8`。
3. 点击“保存配置”和“校验配置”，容量应为 `列=2 行=3 单层=6 总容量=48`。
4. 点击“清零当前区域”，确认当前已放为 `0`。
5. 点击“计算下一点”，确认显示第 `1` 层第 `1` 行第 `1` 列，相对 `Z=0`。
6. 点击“模拟放置完成”，确认当前已放变为 `1`，下一点推进到第 `1` 层第 `1` 行第 `2` 列。
7. 连续点击“模拟放置完成”直到已放数量为 `6`，下一点应进入第 `2` 层，相对 `Z=108`。
8. 点击“仿真 8 层”，表格应生成 `48` 行；默认托盘高为 0 时，第 `1-6` 行 `offsetZ=0`，第 `7-12` 行 `offsetZ=108`，之后每层递增 `108`。
9. 切换到“大箱码垛”页重复步骤 2-8；容量应为 `列=1 行=2 单层=2 总容量=16`，层高递增为 `288`。
10. 分别勾选 `反转 X`、`反转 Y` 后重新“计算下一点”，确认对应方向的相对偏移符号反转。
11. 将托盘 Z 临时改为 `100` 并保存/仿真，确认第一层 `offsetZ=100`，第二层小箱 `offsetZ=208`；验证托盘高度已并入总高度。
12. 点击“清除显示结果”，确认下一点/校验结果/8 层仿真表格被清空，但容量和已放数量不变。
13. 点击“检测当前区域”验证 HTTP 视觉占用结果；失败时不得修改缓存，连续 3 次识别为空才允许自动清零。

### 后续真实流程步骤

1. 主流程根据扫码、客户系统或工位配置确定 `PalletArea`。
2. 进入码垛步骤前，可先把 AGV/相机移动到能观察码垛区的位置，并调用视觉占用检测。
3. 若视觉连续识别为空且不在机械臂动作中，则清零缓存；若识别到目标，则沿用缓存。
4. 调用 `nextRelativeOffset(area)` 获取相对机械臂初始点位的偏移。
5. 机械臂先运动到现场示教的码垛初始点位，再按相对偏移移动到释放点正上方。
6. 机械臂松开夹爪，箱体自由落体到目标位置。
7. 收到机械臂“到点 + 松爪完成”确认后，调用 `commitPlaced(area)` 推进缓存。
8. 若 `commitPlaced()` 返回满载，暂停码垛并通知人工搬运；若现场中途被叉车搬走，优先通过视觉空区防抖清零或人工修正已放数量。

---

## 阶段五：接入 MainWindow 与 CMake

**文件：**
- 修改：`CMakeLists.txt`
- 修改：`src/mainwindow.h`
- 修改：`src/mainwindow.cpp`

- [x] **步骤 5.1：CMake 加入新增文件**

在 `PROJECT_SOURCES` 加入：

```cmake
src/palletscheduler.cpp
src/palletscheduler.h
src/palletparamdialog.cpp
src/palletparamdialog.h
```

保持 `CMAKE_AUTOMOC ON`，不需要新增 Qt 模块。

- [x] **步骤 5.2：MainWindow 声明成员**

`mainwindow.h` 前置声明：

```cpp
class PalletScheduler;
class PalletParamDialog;
```

成员区增加：

```cpp
PalletScheduler *m_palletScheduler = nullptr;
QPushButton *m_btnPalletConfig = nullptr;
QPointer<PalletParamDialog> m_palletDialog;
```

private slots 增加：

```cpp
void onPalletConfig();
```

private 方法增加：

```cpp
void initPalletPanel(QVBoxLayout *leftPanel);
```

- [x] **步骤 5.3：MainWindow 构造和入口面板**

构造函数中创建 `m_palletScheduler = new PalletScheduler(this);`，位置放在 `m_devMgr = new DeviceManager(this);` 之后。

`initUI()` 左侧面板功能组中，在 AGV 或华沿 SDK 面板附近调用 `initPalletPanel(leftPanel)`。面板内容：

```text
QGroupBox("码垛配置")
  QPushButton("打开码垛配置")
```

按钮高度 28-30px，objectName 可设为 `btnPalletConfig`，样式交给全局 QSS。

- [x] **步骤 5.4：打开配置窗口**

`onPalletConfig()` 使用单例窗口，避免重复打开：

```cpp
if (m_palletDialog) {
    m_palletDialog->raise();
    m_palletDialog->activateWindow();
    return;
}

auto *dialog = new PalletParamDialog(
    m_palletScheduler,
    m_devMgr->visionClient(),
    this);
m_palletDialog = dialog;
dialog->setAttribute(Qt::WA_DeleteOnClose);
connect(dialog, &QObject::destroyed, this, [this] { m_palletDialog = nullptr; });
dialog->show();
```

- [x] **阶段五验证**

1. CMake configure/build 成功。
2. 主窗口左侧出现“码垛配置”面板。
3. 点击按钮可打开配置窗口，重复点击只激活已有窗口。
4. 关闭窗口不影响主窗口。
5. 现有网络配置、相机、扫码器、N-ScanHub、客户系统、AGV、华沿 SDK 面板行为不变。

---

## 阶段六：集成验证、流程闭环和现场 dry-run

**文件：**
- 不新增文件；按验证结果只修正前述文件。

- [x] **步骤 6.1：编译验证**

在当前工程可用构建目录执行 CMake/build。预期无 error。若本机缺少硬件 SDK 运行条件，本功能仍应能完成编译，因为新增内容只依赖 Qt Core/Widgets/Network。

- [x] **步骤 6.2：UI 回归验证**

手动验证：

1. 主窗口启动正常。
2. 打开码垛配置窗口。
3. 大箱/小箱分别输入不同参数并保存。
4. 关闭重开后配置和已放数量恢复。
5. 深色/浅色主题切换后对话框仍可读。
6. 现有各设备调试面板不受影响。

- [x] **步骤 6.3：算法验收数据验证**

使用现场模拟参数核对：

- 小框：托盘 1100x1100，小框 450x270x108，间距/边距 20，结果应为 2 列、3 行、6/层、48/8层。
- 大框：托盘 1100x1100，大框 580x380x288，间距/边距 20，结果应为 1 列、2 行、2/层、16/8层。
- 第 1 箱在负 X 负 Y 角。
- 一层最后一箱后进入下一层。
- 方向反转后点位符号正确。
- 满载后报错。

- [x] **步骤 6.4：视觉 HTTP 验证**

使用视觉服务 `/inference` 验证：

1. 视觉未配置或服务断开时，UI 显示检测失败，不修改缓存。
2. `object_count > 0` 时，UI 显示占用，连续空计数清零。
3. `object_count == 0` 时，UI 显示空，第 1/2 次只累计，第 3 次自动清零。
4. 码垛执行中 `setStackingActive(area,true)` 时，即使检测为空也不自动清零。
5. 占用检测不触发机械臂抓取偏移信号。

- [x] **步骤 6.5：完整操作流程检查**

v1 的人工/调试闭环不对接机械臂真实完成信号。配置窗口提供“模拟放置完成”按钮，用于在仿真时手动调用 `commitPlaced()` 推进缓存；真实主流程未来仍必须等待机械臂完成回调后再提交：

```text
打开码垛配置
→ 填入默认现场值或手动输入参数
→ 保存配置
→ 校验配置
→ 视觉检测当前区域
→ 若连续 3 次为空则自动清零，否则沿用缓存
→ 计算下一点
→ 机械臂 dry-run 或后续主流程执行
→ 仿真时点击“模拟放置完成”推进缓存；真实流程未来由机械臂到点并松爪完成后 commitPlaced
→ 刷新下一点/容量状态
```

确认每个箭头都有明确按钮、接口或状态显示，不存在只能靠开发者猜测的步骤。

- [x] **步骤 6.6：现场 dry-run 清单**

现场按顺序验证：

1. 示教大箱区中心点，保存 `originPose`。
2. 示教小箱区中心点，保存 `originPose`。
3. 用“计算下一点”移动到第一箱释放点，不松爪。
4. 若方向不对，调整 `invertX/invertY`。
5. 确认 `releaseZOffset` 高度安全。
6. 使用视觉检测当前区域，确认 occupied/empty 与现场一致。
7. 单箱实放本期只观察结果，不对接机械臂完成信号；仿真验证时点击“模拟放置完成”推进已放数量。
8. 仿真 8 层，确认层高和容量符合现场预期。

- [x] **阶段六完成标准**

功能满足：配置可保存、点位可计算、配置窗口可模拟推进放置状态、状态可人工修正、视觉 HTTP 可检测并防抖清零、满载可提示、主流程未被改变；机械臂真实放置完成提交留作未来 TODO。

---

## Future TODO

以下能力不在本期实现范围内，但需要作为后续接入方向保留：

- **机械臂放置完成提交：** 主流程调用 `nextPose()` 获取点位后，驱动机械臂移动到释放点并松开夹爪；只有收到机械臂“到点 + 松爪完成”确认后，才调用 `commitPlaced()`。
- **主流程码垛步骤接入：** 在 `LineOrchestrator` 或后续流程编排中新增“放置空箱码垛”步骤，处理满载、视觉为空自动清零、点位获取失败、机械臂执行失败等分支。
- **箱型来源接入：** 明确 `PalletArea` 来源，可来自扫码、客户系统、工位配置或 UI 手动选择；`PalletScheduler` 不负责推断箱型。
- **视觉位姿前置动作：** 视觉占用检测前，需要 AGV/机械臂/相机处于能观察对应码垛区的位置；后续流程需补齐“移动到检测位 -> 调 HTTP -> 根据结果处理状态”的动作。
- **真实放置后视觉复核：** 机械臂松爪后可再次调用视觉检测确认目标存在，作为 `commitPlaced()` 前的二次保险；首版不依赖该复核。
- **部分搬走自动纠偏：** 当前只支持人工修正已放数量；未来可结合视觉高度/数量估计做半自动纠偏。
- **箱体旋转优化：** 当前不旋转箱体；未来可计算 `boxSize.x/y` 互换后的容量，并在 UI 中提供“固定方向/自动优化/手动旋转”选项。
- **路径安全与避障：** 当前只输出释放点，不规划机械臂从当前位姿到释放点的安全路径；未来需结合机械臂侧轨迹或中间过渡点。
- **工人搬运通知：** 满载时当前只发 `areaFull` 和 UI 提示；未来可接声光、客户系统、AGV 调度或工位状态上报。
- **运行日志与追溯：** 后续可记录每次点位、箱型、层/行/列、视觉结果、机械臂执行结果和人工修正记录。

---

## 后续主流程接入预留

本期不实现，但接口按以下方式预留：

```cpp
PalletPose target; // 优先表示相对初始点位的偏移
QString error;
if (!m_palletScheduler->nextRelativeOffset(PalletArea::LargeBox, &target, &error)) {
    // 暂停流程或提示人工处理
    return;
}

m_palletScheduler->setStackingActive(PalletArea::LargeBox, true);
// 机械臂先到初始点位，再按 target 相对偏移移动并松爪
m_palletScheduler->setStackingActive(PalletArea::LargeBox, false);

if (armPlaceSuccess) {
    m_palletScheduler->commitPlaced(PalletArea::LargeBox, &error);
}
```

接入方必须决定 `PalletArea` 来源：扫码、客户系统、工位配置或 UI 手动选择。`PalletScheduler` 不推断箱型。

## Assumptions

- 箱子方向固定，不做旋转优化。
- `originPose` 是机械臂初始点位，主要用于绝对预览；主流程优先使用相对偏移。
- 第一箱默认从 `负X、负Y` 角开始。
- 每层先沿 `X` 方向排布，再沿 `Y` 换行。
- 本期不对接机械臂执行结果；未来放置成功由机械臂“到点 + 松爪完成”确认，确认后才更新缓存。
- 视觉 `/inference` 的 `object_count > 0` 表示当前相机视野内有空箱/目标，`object_count == 0` 表示当前视野为空。
- 视觉检测前，AGV/相机必须已经对准对应码垛区；否则 HTTP 结果不能代表真实码垛区状态。
- 视觉自动清零阈值为连续 3 次识别为空。
- 视觉自动清零只能在非码垛执行中触发。
- 叉车只搬走部分箱子的情况由人工修正已放数量处理。
- 当前箱型/码垛区由调用方显式传入，不由 `PalletScheduler` 推断。
- v1 把 `palletSize.z` 自动加入相对 Z 偏移和最高安全 Z 校验；`releaseZOffset` 仍只是现场推荐/记录值，不自动叠加。
- 本阶段只做功能模块、配置 UI、视觉占用检测和接口预留，不把码垛动作接入现有整线主流程；配置窗口可手动模拟放置完成，真实机械臂完成信号仍待后续接入。


## 已实现 UI 输入防误触说明

- 项目源码中已移除 `QSpinBox` 直接使用，整数输入改为 `QLineEdit + QIntValidator`。
- 码垛配置窗口的小数输入使用屏蔽滚轮的 `NoWheelDoubleSpinBox`，鼠标滚轮不会修改尺寸、坐标或高度值。
- 若后续新增数值输入控件，必须沿用以上策略，避免鼠标轻微滑动导致参数变化。
