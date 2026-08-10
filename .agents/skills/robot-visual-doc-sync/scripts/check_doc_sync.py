#!/usr/bin/env python3
"""检查 robot_visual 代码差异与长期项目文档是否同步。

脚本只做确定性检查：根据 Git 变更路径给出候选文档，并在严格模式下要求
相关代码提交同时包含项目文档更新。业务语义是否真正一致仍需开发者或智能体审阅。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


PROJECT_DOC_DIR = Path("docs/project-knowledge")
ENTRY_DOC = PROJECT_DOC_DIR / "项目认知入口.md"
INVARIANT_DOC = PROJECT_DOC_DIR / "关键业务不变量与现场约束.md"

# 每条规则把代码或测试路径映射到需要人工核对的长期文档。
# 使用正则而不是固定完整文件列表，确保新增同类测试也能进入检查范围。
IMPACT_RULES: tuple[tuple[re.Pattern[str], tuple[str, ...]], ...] = (
    (
        re.compile(r"^src/(linemanager|taskexecutor|taskqueue|lineconfig)\."),
        ("核心调度与业务流程.md", "状态机与安全约束.md", "关键业务不变量与现场约束.md"),
    ),
    (
        re.compile(r"^src/(huayanScheduler|visionclient|visionalignmentdecision)\."),
        (
            "华沿机械臂视觉抓取与运动判断.md",
            "状态机与安全约束.md",
            "关键业务不变量与现场约束.md",
            "现场运行与故障排查手册.md",
            "外部接口信号与数据字典.md",
        ),
    ),
    (
        re.compile(r"^src/agvcontroller\."),
        (
            "核心调度与业务流程.md",
            "状态机与安全约束.md",
            "现场运行与故障排查手册.md",
            "外部接口信号与数据字典.md",
            "关键业务不变量与现场约束.md",
        ),
    ),
    (
        re.compile(r"^src/(nscanscheduler|palletscheduler|palletplacesequence)\."),
        (
            "核心调度与业务流程.md",
            "状态机与安全约束.md",
            "现场运行与故障排查手册.md",
            "外部接口信号与数据字典.md",
        ),
    ),
    (
        re.compile(r"^src/(autochargecoordinator|chargepile|chargebusinessrules|chargesettings|chargeshutdownpolicy)"),
        (
            "核心调度与业务流程.md",
            "状态机与安全约束.md",
            "现场运行与故障排查手册.md",
            "外部接口信号与数据字典.md",
            "关键业务不变量与现场约束.md",
        ),
    ),
    (
        re.compile(r"^src/(customSysScheduler|devicemanager)\."),
        (
            "系统架构与模块职责.md",
            "核心调度与业务流程.md",
            "外部接口信号与数据字典.md",
            "关键业务不变量与现场约束.md",
        ),
    ),
    (
        re.compile(r"^src/networkcompat\.h$"),
        (
            "项目认知入口.md",
            "系统架构与模块职责.md",
            "代码符号测试索引与维护规则.md",
        ),
    ),
    (
        re.compile(r"^tests/"),
        ("代码符号测试索引与维护规则.md",),
    ),
)

# 尚未登记到精确规则的新源码模块先映射到四份全局认知文档。
# 这是一条保守兜底：首次新增功能不能静默绕过文档同步；完成功能时还应把
# 新模块加入上面的精确规则并增加测试，以便后续只读取真正相关的专题文档。
GENERIC_SOURCE_PATTERN = re.compile(r"^src/.+\.(?:h|hpp|c|cc|cpp|cxx)$")
GENERIC_SOURCE_DOCUMENTS = (
    "项目认知入口.md",
    "系统架构与模块职责.md",
    "代码符号测试索引与维护规则.md",
    "关键业务不变量与现场约束.md",
)


def run_git_name_only(arguments: list[str]) -> list[str]:
    """执行 Git 路径查询并返回使用正斜杠的仓库相对路径。"""

    result = subprocess.run(
        # 关闭路径转义，确保中文长期文档名称能够被 changed_docs 精确识别。
        ["git", "-c", "core.quotepath=false", *arguments],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if result.returncode != 0:
        print(result.stderr.strip() or "Git 差异查询失败。", file=sys.stderr)
        raise SystemExit(2)
    return [line.strip().replace("\\", "/") for line in result.stdout.splitlines() if line.strip()]


def changed_files(mode: str) -> dict[str, str]:
    """按检查模式返回“路径到 Git 状态”的映射，并识别新增的未跟踪文件。"""

    if mode == "staged":
        lines = run_git_name_only(
            ["diff", "--cached", "--name-status", "--diff-filter=ACMR"]
        )
    else:
        lines = run_git_name_only(["diff", "--name-status", "--diff-filter=ACMR", "HEAD"])

    changes: dict[str, str] = {}
    for line in lines:
        fields = line.split("\t")
        status = fields[0]
        # Git 重命名记录包含“状态、旧路径、新路径”，文档影响应按新路径判断。
        path = fields[-1]
        changes[path] = status

    if mode == "working-tree":
        for path in run_git_name_only(["ls-files", "--others", "--exclude-standard"]):
            changes[path] = "A"
    return dict(sorted(changes.items()))


def impacted_documents(
    paths: list[str], added_paths: set[str] | None = None
) -> dict[str, set[str]]:
    """生成文档到触发源文件的反向映射，便于报告影响原因。"""

    added_paths = added_paths or set()
    impacts: dict[str, set[str]] = {}
    for path in paths:
        matched = False
        for pattern, documents in IMPACT_RULES:
            if pattern.search(path):
                matched = True
                for document in documents:
                    impacts.setdefault(document, set()).add(path)
        # 兜底仅针对 Git 能确认的新增源码。这样新模块不会漏掉文档同步，
        # 同时普通界面样式等既有文件的修改不会被误判为新业务功能。
        if not matched and path in added_paths and GENERIC_SOURCE_PATTERN.search(path):
            for document in GENERIC_SOURCE_DOCUMENTS:
                impacts.setdefault(document, set()).add(path)
    return impacts


def validate_long_term_documents() -> list[str]:
    """验证长期文档的稳定命名、占位符和 Markdown 围栏。"""

    errors: list[str] = []
    if not PROJECT_DOC_DIR.is_dir():
        return [f"长期文档目录不存在：{PROJECT_DOC_DIR}"]

    dated_name = re.compile(r"^\d{4}-\d{2}-\d{2}-")
    placeholders = re.compile(r"\b(?:TBD|TODO)\b|待补充|待完善")
    for path in sorted(PROJECT_DOC_DIR.glob("*.md")):
        if dated_name.match(path.name):
            errors.append(f"长期文档文件名不得带日期：{path}")
        text = path.read_text(encoding="utf-8")
        if placeholders.search(text):
            errors.append(f"长期文档包含占位符：{path}")
        if text.count("```") % 2:
            errors.append(f"Markdown 围栏未闭合：{path}")
        if "project-knowledge/20" in text:
            errors.append(f"长期文档包含疑似旧日期链接：{path}")

    for required in (ENTRY_DOC, INVARIANT_DOC):
        if not required.is_file():
            errors.append(f"必需文档不存在：{required}")
    return errors


def parse_args() -> argparse.Namespace:
    """解析命令行参数，严格限制一次只选择一种差异来源。"""

    parser = argparse.ArgumentParser(description="检查 robot_visual 代码与长期项目文档同步情况")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--staged", action="store_true", help="检查已暂存、即将提交的差异")
    mode.add_argument("--working-tree", action="store_true", help="检查工作区相对 HEAD 的全部差异")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true", help="不满足同步规则时返回非零退出码")
    action.add_argument("--report", action="store_true", help="只报告影响范围，不强制文档已更新")
    parser.add_argument(
        "--allow-no-doc-change",
        metavar="原因",
        help="确认相关改动不改变行为或接口；严格模式下允许不修改长期文档",
    )
    return parser.parse_args()


def main() -> int:
    """报告影响范围，并在严格模式下执行文档同步门禁。"""

    args = parse_args()
    mode = "working-tree" if args.working_tree else "staged"
    strict = args.check
    changes = changed_files(mode)
    paths = list(changes)
    added_paths = {path for path, status in changes.items() if status.startswith("A")}
    impacts = impacted_documents(paths, added_paths)
    changed_docs = {
        Path(path).name
        for path in paths
        if path.startswith(f"{PROJECT_DOC_DIR.as_posix()}/") and path.endswith(".md")
    }

    print(f"差异模式：{'工作区' if mode == 'working-tree' else '已暂存'}")
    print(f"变更文件：{len(paths)}")
    if impacts:
        print("需要核对的长期文档：")
        for document, sources in sorted(impacts.items()):
            print(f"- {document}")
            for source in sorted(sources):
                print(f"  - 触发：{source}")
    else:
        print("未发现映射到核心项目文档的代码或测试变化。")

    errors = validate_long_term_documents()
    if strict and impacts and not changed_docs:
        reason = (args.allow_no_doc_change or "").strip()
        if len(reason) < 10:
            errors.append(
                "核心代码或测试已变化，但没有暂存长期文档；"
                "请更新文档，或使用 --allow-no-doc-change 提供不少于10个字符的具体原因。"
            )
        else:
            print(f"文档更新豁免：{reason}")

    if strict and impacts and changed_docs:
        missing_updates = sorted(set(impacts) - changed_docs)
        if missing_updates:
            print("以下候选文档未修改，需要人工确认确实不受影响：")
            for document in missing_updates:
                print(f"- {document}")

    if errors:
        print("检查失败：", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    print("文档同步检查通过。" if strict else "影响范围报告完成。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
