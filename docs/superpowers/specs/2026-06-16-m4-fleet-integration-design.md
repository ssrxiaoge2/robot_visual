# M4 车队调度接入与多车工位互斥 设计文档

日期：2026-06-16
分支：`feature/m4-fleet-integration`（基于 master）
依据：《M4 调度 API 文档》《M4 HTTP 接口文档》（仙工 M4 车队调度系统）

## 背景与目标

车间存在多台 AGV。现有上位机通过 **Modbus 直连单台 AGV** 派单（`AgvController` → `sendToStation`），无法做多车交通管制。需求：

- A 车在 A 工位倒料期间，任何要**经过 A 工位**的车都不能进入，必须等倒料完成；
- 反之，有车正在经过 A 工位时，A 车也不能进入 A 工位倒料。

仙工现场已部署 **M4 车队调度系统**。M4 自带多车交管/避让，且提供「运单」与「外部地图资源申请」两套 HTTP 接口，恰好覆盖本需求。

**核心思路**：

1. 整线流程的 AGV 侧由「Modbus 直连单车派单」改为「向 M4 下运单」，选车、路径、避让交给 M4；
2. A 工位倒料这种「车停着、由上位机控机械臂作业」的固定占用期，用 M4「外部地图资源申请/释放」把 A 工位区域显式锁住，作业完释放，实现严格互斥。

机械臂侧（华沿 SDK，`HuayanScheduler` 取料/收姿态/倒料）**逻辑不变**，仅 AGV 侧改造。

## M4 接口要点（摘自文档）

- 基础：HTTP REST，请求头鉴权 `xyy-app-id` / `xyy-app-key`（M4「代理用户」菜单创建）；连通性 `GET /api/ping`。WebSocket `ws://{ip}:{port}/wsm`（本期不用，见范围外）。
- 运单：
  - 建单 `POST /api/fleet/orders/create`（`sceneId` + `steps[]`，步骤含 `location`、`rbkArgs.binTask`、`forLoad/forUnload`、`stepFixed` 封口位）→ 返回 `orderId`
  - 加步 `POST /api/fleet/orders/add-steps`（未封口运单追加步骤）
  - 封口完成 `POST /api/fleet/orders/complete-order`
  - 取消 `POST /api/fleet/orders/cancel`
  - 查详情 `GET /api/fleet/orders/query-order-detail?orderId=xxx`：运单状态 `ToBeAllocated/Allocated/Pending/Executing/Done/Cancelling/Cancelled`，步骤状态 `Executable/Executing/Done/Cancelled`
- 外部地图资源（互斥锁）：
  - 申请 `POST /api/fleet/external-map-res/request`（`unitId` 唯一、`owner`、`sceneId`、`pointNames`/`pathKeys`/`spatialZones` 多边形）→ `ok` true/false，失败返回冲突的已占资源
  - 释放 `POST /api/fleet/external-map-res/release`（按 `unitId`）/ `release-by-owner`（按 owner 兜底清理）
  - 规则：「谁申请谁释放」；一次申请同一场景同一区域同一 owner

## 整体流程（单次，M4 运单驱动）

整线 = **一条不提前封口的运单**，AGV 到位后停留由上位机控机械臂作业，作业完再追加下一步：

```
0. 初检        ping M4 + 机械臂强制收姿态（AGV 在位由 M4 调度，不再本地判站）
1. 建运单(去取料工位)  create(stepFixed=false, step=取料 location)；M4 选车 → 等步骤 Done（到达取料位）
2. 机械臂取料   startStageOne（视觉闭环夹取），AGV 被未封口运单占住、停在取料位不被改派
3. 收姿态      startStow
4. 加步(去倒料工位A)  add-steps(step=倒料 location)；等步骤 Done（到达 A 工位）
5. 锁 A 工位   external-map-res/request（owner=外部系统名，unitId=UUID，锁 A 工位区域）
6. 倒料        startUnload（func_daoliao_1_point → func_daoliao）
7. 解锁 A 工位  external-map-res/release（按本次 unitId）
8. 收姿态      startStow
9. 加步(回待机)+封口  add-steps(回待机 location) + complete-order；等运单 Done
→ 完成，手动重跑。任一步出错就地停机 + 释放本次所持的锁
```

要点：
- 步骤 1/4 期间「运单未封口」是关键——运单到达后处于 `Pending`，机器人被该运单占住、M4 不会改派去别的活，从而**停在工位等上位机机械臂作业**。
- 锁的时序：**到达 A 工位之后再申请、离开之前先释放**。若提前在到达前申请，会把自己这台车也挡在外面；故顺序固定为「到位 → 锁 → 作业 → 解锁 → 走」。
- 反向互斥自动成立：若别的车正占/锁 A 工位，本车运单步骤到 A 工位的路径规划会被 M4 阻塞等待；若申请锁返回 `ok=false`（区域被占），上位机等待重试至超时。

> 取料工位若也是共享点，同样可在步骤 2 期间用同一套锁机制，按需开启（本期默认只锁倒料 A 工位，取料锁留配置开关）。

## 1. 新增组件：M4Client（`src/m4client.{h,cpp}`）

Qt `QNetworkAccessManager` 异步 HTTP 客户端，封装 M4 运单与地图资源接口。职责单一：只做 HTTP 收发与 JSON 解析，不含业务时序（时序在 `LineOrchestrator`）。

### 配置

```cpp
void setServer(const QString &ip, int port);          // baseUrl = http://ip:port
void setAuth(const QString &appId, const QString &appKey); // 请求头注入
void setScene(const QString &sceneId, const QString &sceneName);
```

### 接口（均异步，结果走信号）

```cpp
void ping();                                           // GET /api/ping
QString createOrder(const QList<OrderStep> &steps, bool stepFixed=false); // 返回本地占位 reqId
void addSteps(const QString &orderId, const QList<OrderStep> &steps);
void completeOrder(const QString &orderId);
void cancelOrder(const QString &orderId);
void queryOrderDetail(const QString &orderId);         // 轮询用
void requestMapRes(const QString &unitId, const QString &owner,
                   const MapResArea &area);            // points/paths/polygon
void releaseMapRes(const QString &unitId);
void releaseByOwner(const QString &owner);             // 兜底清理
```

`OrderStep` 结构：`location`(String) + `binTask`(可空) + `forLoad`/`forUnload`(bool)。
`MapResArea`：`pointNames`(List) / `pathKeys`(List) / `polygon`(List<Point2D>) 三选一或组合。

### 信号

```cpp
void pingResult(bool ok);
void orderCreated(const QString &orderId);
void orderStepDone(const QString &orderId, int stepIndex);
void orderDone(const QString &orderId);
void orderFailedOrCancelled(const QString &orderId, const QString &status);
void mapResResult(const QString &unitId, bool ok, const QString &detail);
void mapResReleased(const QString &unitId);
void m4Error(const QString &msg);   // 网络/HTTP 400/解析错误
void m4Log(const QString &msg);
```

### 完成通知机制

本期采用**轮询**：`createOrder`/`addSteps` 后启 `QTimer`（约 800ms）周期 `queryOrderDetail`，比对运单状态/步骤状态变化 → emit `orderStepDone`/`orderDone`/`orderFailedOrCancelled`。
理由：HTTP 回调需上位机自起 HTTP 服务、WebSocket 需引入 `QtWebSockets` 并管理长连——均增复杂度。轮询实现最简、足够（整线节拍以秒计）。WebSocket 订阅列范围外，后续优化。

## 2. LineOrchestrator 改造

状态机由「Modbus 派单 + monitor 判到达」改为「M4 运单生命周期」。新状态枚举：

```
Idle / InitCheck /
ToPickup(等步骤Done) / ArmPicking / StowAfterPick /
ToUnload(等步骤Done) / LockUnloadZone(等mapRes ok) / ArmUnloading / UnlockUnloadZone /
StowAfterUnload / ToHome(等运单Done)
```

- 构造注入 `M4Client*` + `HuayanScheduler*`（替换原 `AgvController*`），由 `DeviceManager` 持有注入。
- 工位→M4 location 的解析继续走注入的解析器（复用 `setStationResolver` 思路，改为返回 location 字符串），见第 3 节。
- 推进：监听 `M4Client::orderStepDone`/`orderDone` 推进 AGV 步；监听 `HuayanScheduler::stageCompleted` 推进机械臂步；`orderFailedOrCancelled`/`m4Error`/`stageError` → `abort`。
- 锁 owner 用固定外部系统名（如 `WHRobotUpper`，配置项，不得与车重名）；`unitId` 每次倒料生成 UUID，记于成员供释放/兜底。
- 对外信号 `lineStarted/lineFinished/lineStopped/lineError/stepChanged/lineLog` 保持不变（MainWindow/流程图无需改动）。

原 `agvDispatchRequested` 信号、`monitorUpdated` 到达判定、`m_expectedStation`/`m_agvSeenMoving`/站点号解析在 M4 路径下废弃。

## 3. DeviceManager / 配置 / MainWindow

- `DeviceManager`：新增持有 `M4Client *m_m4`，构造时按配置 `setServer/setAuth/setScene`；`LineOrchestrator` 改注入 `m_m4`；转发 `m4Log/m4Error` 到 `logMessage`。
- 工位映射：现「工位号→AGV 站点号」(`m_stationMap`) 改/并存为「工位号→M4 location 名」(`m_locationMap`，值为字符串如 `BHQ1_0101_03`)。`resolveLocation(workstation)` 注入编排器。
- `DeviceManager::Config` 增字段：`m4IP`、`m4Port`、`m4AppId`、`m4AppKey`、`m4SceneId`、`m4SceneName`、`unloadLockOwner`、A 工位锁区域（点位/路径/多边形）。
- `MainWindow` 配置面板：增「M4 调度」分组（IP/端口/AppId/AppKey/场景）输入与「测试」按钮（调 `ping`）。工具栏开始/停止、流程图驱动不变。

### AgvController 与现有 AGV 调试面板

M4 模式下，**上位机不得再向 AGV 直写 Modbus 派单**（与 M4 控车权冲突，双主控危险）。处理：

- `AgvController` 与「AGV 调试面板」保留作单机离线调试工具，但其**直接派单/取消能力在 M4 模式下禁用**（灰显或加模式开关）；监控读取可保留。
- 整线流程不再使用 `AgvController`。

## 4. 错误处理与互斥锁安全

- `haltDevices()`：`m4->cancelOrder(当前orderId)` + `m4->releaseByOwner(锁owner)`（兜底确保 A 工位锁一定释放）+ `arm->stop()` + 回 `Idle`。
- `abort(reason)`：`haltDevices()` + emit `lineError`。触发源：`orderFailedOrCancelled`、`m4Error`、`stageError`、锁申请超时。
- `stop()`（手动）：`haltDevices()` + emit `lineStopped`。
- **锁泄漏防护（最关键）**：任何中止/停止/异常路径都必须释放本次锁；除按 `unitId` 释放外，停机统一再 `release-by-owner` 兜底，避免 A 工位被永久占死。
- 锁申请失败（`ok=false`，区域被占）：等待重试，超时（如 60s）`abort`。
- 单步超时：AGV 步用本地超时保护（如 180s 运单未 Done 则 `abort`）；机械臂步沿用 `HuayanScheduler` 现有超时。

## 5. 需现场/仙工确认的配置与待验证点

配置项（落地填写，代码留占位与 setter）：

| 项 | 说明 |
|----|------|
| M4 IP / 端口 | M4 服务地址 |
| app-id / app-key | M4「代理用户」创建 |
| sceneId / sceneName | 目标场景 |
| 工位→location 名 | 取料/倒料/待机各工位对应 M4 点位名或库位名 |
| binTask 名 | 库位上 Load/Unload 的实际名称（若用 binTask 取放） |
| A 工位锁区域 | 锁的 pointNames/pathKeys，或多边形顶点坐标（现场地图） |
| 锁 owner 名 | 外部系统名，唯一且不与车重名 |

待仙工/现场验证：

1. 车停在 A 工位（被未封口运单占住）期间，对该区域 `external-map-res/request`（owner=外部名）是否会因自车在区内而失败——预期：自车已先到位，锁针对**其他**车，应可成功；需实测。
2. 「未封口运单 + Pending 停留」期间 M4 是否会因充电/避让把本车调走——若会，则需改用「分腿独立运单 + 到位后立即锁住车位资源」方案（备选，见下）。
3. 运单步骤 `Done` 的时机是否=「车到点并停稳」（决定何时开始机械臂作业）。

备选方案（若验证点 2 不成立）：每段 AGV 行程一条独立封口运单，到位后立即用 external-map-res 锁住「当前车位点」防被调走，机械臂作业完释放再下一段运单。复杂度更高，仅在 Pending 停留不可靠时启用。

## 6. 测试

1. 全量编译（MinGW Debug）零警告。
2. 离线：M4 不可达时点「开始」→ 看 ping 失败/错误链路；mock 一个最小 M4（本地 HTTP 桩）验证建单/查状态/锁申请释放的报文与状态推进。
3. **真机验收（主）**：单台车跑通整线（运单建/加步/封口、到位、机械臂作业、A 工位锁/解锁顺序、日志与流程图）；再两台车验证互斥——A 车倒料期间令 B 车运单途经 A 工位，观察 B 被挡、A 完成解锁后 B 放行；测中途「停止」与人为故障下锁被正确释放。

## 范围外（本期不做）

- WebSocket 实时订阅（用轮询替代）；HTTP 回调服务端。
- 上位机侧多车并行编排（多车交管由 M4 负责，上位机只管自己这条线/这张运单）。
- 充电/停靠/避让单管理（M4 自理）。
- 删除 `AgvController` 与 AGV 调试面板（仅在 M4 模式禁用其派单）。
- 工位取料锁默认关闭（仅留配置开关）。
