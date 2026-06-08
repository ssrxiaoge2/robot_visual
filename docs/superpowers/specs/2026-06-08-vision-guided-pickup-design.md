# 视觉引导取料设计规格

**日期**：2026-06-08  
**状态**：待实现  
**范围**：VisionHttpClient、HuayanScheduler、DeviceManager

---

## 1. 目标

机器人（华沿 SDK 路径）能够根据视觉服务返回的 XYZ 偏移量和旋转角度，自动运动到料框上方并完成夹取。整个流程分为：移到拍照位 → 视觉拍照 → 移到抓取位 → 夹爪闭合 → 抬升。

---

## 2. 约束与前提

- 使用华沿原生 SDK 路径（`HuayanScheduler`），不修改 Modbus 路径（`WorkflowEngine` + `RobotController`）
- 视觉坐标在**工具坐标系（UCS="TCP"）**下以 `double` mm 表示
- 坐标注入由外部（`DeviceManager`）通过信号槽完成，`HuayanScheduler` 不直接持有 `VisionHttpClient`
- 现有 `coordinatesReady(QList<quint16>)` 信号保留，Modbus 路径不受影响

---

## 3. 数据流

```
HuayanScheduler          DeviceManager（连线）         VisionHttpClient
      |                         |                              |
  MoveToSurvey 完成             |                              |
      |── surveyReady() ──────> |── fetchInference() ────────> |
      |                         |                              | 解析 JSON
      |                         |                              | 手眼变换 → double mm
      |                         | <── rawCoordinatesReady() ── |
      | <── setGrabOffset() ─── |                              |
  MoveToGrab(UCS="TCP")
      |
  CloseGripper（1.5s 固定等待）
      |
  LiftLoad(UCS="Base")
      |
  stageCompleted("阶段一：取料")
```

---

## 4. VisionHttpClient 变更

### 4.1 新增信号

```cpp
void rawCoordinatesReady(double x, double y, double z, double rz);
```

| 参数 | 含义 | 单位 |
|------|------|------|
| x    | 工具坐标系 X 偏移 | mm |
| y    | 工具坐标系 Y 偏移 | mm |
| z    | 工具坐标系 Z 偏移（深度） | mm |
| rz   | 规范化旋转角，映射到 (-90°, 90°] | 度 |

rx、ry 固定为 0（不改变俯仰/横滚），不在信号中传递。

### 4.2 内部重构：拆分 transformToRegisters

新增私有结构体和方法：

```cpp
struct RawCoords { double x, y, z, rz; };
RawCoords transformToMm(float cx, float cy, float cz, float angleDeg);
```

`transformToMm` 执行：
1. 手眼矩阵变换：相机坐标系 → 工具坐标系（mm）
2. 角度规范化：`angle` → `rz` ∈ (-90°, 90°]

原 `transformToRegisters` 改为调用 `transformToMm` 后执行 ×1000 + quint16 打包，逻辑不变。

`parseInferenceReply` 在解析完成后同时 emit 两个信号：
- `rawCoordinatesReady(raw.x, raw.y, raw.z, raw.rz)`
- `coordinatesReady(transformToRegisters(...))`（原有，保留）

---

## 5. HuayanScheduler 变更

### 5.1 新增 StageStep 枚举值

```
MoveToSurvey         // 新增：移到拍照位（Base 坐标系）
WaitForVision        // 新增：等待外部注入视觉坐标（不启动任何定时器）
MoveToGrab           // 新增：移到抓取位（TCP 坐标系，由视觉决定）
MoveToPickup         // 保留（可用于无视觉场景的固定抓取）
CloseGripper         // 保留
LiftLoad             // 保留
```

### 5.2 StageOne 完整步骤序列

```
MoveToSurvey   → executeMoveJ(m_surveyPose, UCS="Base") → 轮询 nMovingState==0
WaitForVision  → emit surveyReady() → 挂起（等待 setGrabOffset 调用）
MoveToGrab     → executeMoveJ(m_grabOffset, UCS="TCP") → 轮询 nMovingState==0
CloseGripper   → HRIF_SetToolMotion(close) → QTimer::singleShot(1500ms)
LiftLoad       → executeMoveJ(m_pickupLiftPose, UCS="Base") → 轮询 nMovingState==0
None           → emit stageCompleted("阶段一：取料")
```

### 5.3 新增公有接口

```cpp
// 设置拍照位（Base 坐标系绝对位置，需联机示教后填入）
void setSurveyPose(const Pose &p);

// 接收视觉结果，驱动进入 MoveToGrab（由 DeviceManager 连线调用）
void setGrabOffset(double x, double y, double z, double rz);
```

### 5.4 新增信号

```cpp
void surveyReady();   // 已到拍照位，通知外部触发视觉推理
```

### 5.5 executeMoveJ 扩展

增加 `ucsName` 参数，默认 `"Base"`：

```cpp
bool executeMoveJ(double x, double y, double z,
                  double rx, double ry, double rz,
                  const QString &cmdId  = QStringLiteral("0"),
                  const QString &ucsName = QStringLiteral("Base"));
```

MoveToGrab 步骤调用时传入 `"TCP"`。

### 5.6 新增私有成员

```cpp
Pose m_surveyPose;    // 拍照位（Base 坐标系，需示教后配置）
Pose m_grabOffset;    // 视觉注入的抓取偏移（TCP 坐标系）
```

---

## 6. DeviceManager 变更

### 6.1 新增成员

```cpp
HuayanScheduler *m_huayanScheduler = nullptr;
```

公开访问器：
```cpp
HuayanScheduler *huayanScheduler() const { return m_huayanScheduler; }
```

### 6.2 Config 新增字段

```cpp
QString huayanIP   = QStringLiteral("192.168.10.10");
quint16 huayanPort = 10003;
```

### 6.3 applyConfig() 中的信号连线

```cpp
connect(m_huayanScheduler, &HuayanScheduler::surveyReady,
        m_visionClient,    &VisionHttpClient::fetchInference);

connect(m_visionClient,    &VisionHttpClient::rawCoordinatesReady,
        m_huayanScheduler, &HuayanScheduler::setGrabOffset);
```

`connectRobot()` / `disconnectRobot()` 不在 `applyConfig()` 中自动调用，由 MainWindow 按钮显式触发，与现有设备保持一致。

---

## 7. 联机调试 TODO

- `m_surveyPose`：需通过示教器确认拍照位的六维坐标（Base 坐标系），填入 `setSurveyPose()`
- `m_pickupLiftPose`：需确认抬升后的安全高度坐标
- 验证 UCS="TCP" 下 HRIF_MoveJ 的 x/y/z/rz 方向与实际运动方向是否一致（如方向相反，在 `setGrabOffset` 中取反对应分量）
- 验证视觉 rz 与夹爪物理旋转方向是否一致（参考 VisionHttpClient 中已有的方向说明）

---

## 8. 不在本次范围内

- WorkflowEngine（Modbus 路径）不做任何修改
- 无视觉场景下的固定抓取（`MoveToPickup`）暂不集成 SDK 路径
- StageTwo / StageThree 不变
