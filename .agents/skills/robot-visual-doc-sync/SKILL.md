---
name: robot-visual-doc-sync
description: Use when modifying or reviewing robot_visual scheduling, Huayan robot motion, vision, AGV, scanning, palletizing, charging, external interfaces, state machines, safety constraints, configuration semantics, or related tests, especially before a final commit.
---

# Robot Visual 项目文档同步

## 核心原则

项目长期文档描述当前代码事实。只更新受影响文档，但最终功能提交不得让文档继续描述旧行为。

## 工作流程

1. 运行 `python .agents/skills/robot-visual-doc-sync/scripts/check_doc_sync.py --working-tree --report`，根据输出确定最小阅读范围。
2. 阅读 `docs/project-knowledge/项目认知入口.md` 和 `docs/project-knowledge/关键业务不变量与现场约束.md`。
3. 只读取报告列出的专题文档、相关代码、直接信号连接和相关测试。
4. 修改代码和测试；开发过程中只记录影响，不反复修改正式文档。
5. 相关测试稳定后、最终验证前，根据最终实现更新受影响文档。
6. 暂存代码、测试和文档。
7. 运行 `python .agents/skills/robot-visual-doc-sync/scripts/check_doc_sync.py --staged --check`。
8. 再运行项目要求的构建和测试，最后创建同一功能提交。

## 更新判定

以下变化必须同步文档：

- 模块职责、所有权、跨线程或信号槽关系；
- 调度顺序、状态、超时、停止、失败或恢复；
- 机械臂 SDK 参数、视觉判断、坐标变换或到位证据；
- AGV、扫码、码垛、充电和外部接口的字段、单位或有效条件；
- 配置语义、业务不变量或测试路由。

纯样式、文案、注释或不改变契约的内部整理可不更新。此时运行检查时添加：

```powershell
python .agents/skills/robot-visual-doc-sync/scripts/check_doc_sync.py `
    --staged --check --allow-no-doc-change "仅内部整理，不改变行为或接口"
```

豁免原因必须具体，并在最终说明中报告；不得用“无需更新”“改动很小”等空泛理由。

## 差异映射

检查脚本是代码文件到候选文档的确定性映射来源。脚本报告的是需要人工核对的最小集合，不替代阅读代码和判断真实行为。

新增尚未登记的 `src` 源码模块时，脚本会保守触发项目入口、系统架构、符号测试索引和业务不变量。该功能的最终提交还必须：

1. 把新模块加入检查脚本的精确差异映射；
2. 增加对应映射单元测试；
3. 将新模块加入项目认知入口和代码符号索引；
4. 按实际业务影响更新调度、设备、数据字典、排障或安全文档。

## 完成条件

- 脚本检查退出码为 0；
- 受影响文档描述最终实现而非原计划；
- 文档中的类、状态、信号、字段、寄存器和测试实际存在；
- 没有日期前缀长期文档、旧链接、占位符或未闭合围栏；
- 代码、测试和文档位于同一最终功能提交。
