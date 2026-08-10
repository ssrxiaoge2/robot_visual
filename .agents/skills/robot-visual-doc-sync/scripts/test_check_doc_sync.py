#!/usr/bin/env python3
"""robot-visual-doc-sync 检查脚本的最小回归测试。"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("check_doc_sync.py")
SPEC = importlib.util.spec_from_file_location("check_doc_sync", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ImpactMappingTest(unittest.TestCase):
    """验证高风险模块能路由到对应长期文档。"""

    def test_huayan_and_vision_changes_include_specialized_documents(self) -> None:
        impacts = MODULE.impacted_documents(
            ["src/huayanScheduler.cpp", "src/visionclient.cpp"]
        )

        self.assertIn("华沿机械臂视觉抓取与运动判断.md", impacts)
        self.assertIn("外部接口信号与数据字典.md", impacts)
        self.assertIn("现场运行与故障排查手册.md", impacts)
        self.assertIn("关键业务不变量与现场约束.md", impacts)

    def test_line_manager_change_includes_scheduling_and_safety(self) -> None:
        impacts = MODULE.impacted_documents(["src/linemanager.cpp"])

        self.assertIn("核心调度与业务流程.md", impacts)
        self.assertIn("状态机与安全约束.md", impacts)
        self.assertIn("关键业务不变量与现场约束.md", impacts)

    def test_unrelated_ui_style_change_has_no_forced_document(self) -> None:
        impacts = MODULE.impacted_documents(["src/themeswitch.cpp"])

        self.assertEqual({}, impacts)

    def test_new_test_routes_to_test_index(self) -> None:
        impacts = MODULE.impacted_documents(
            ["tests/test_new_shortage_signal_contract.cpp"]
        )

        self.assertIn("代码符号测试索引与维护规则.md", impacts)

    def test_network_compat_routes_to_supporting_documents(self) -> None:
        """公共Qt网络兼容层应进入入口、架构和符号索引。"""

        impacts = MODULE.impacted_documents(["src/networkcompat.h"])

        self.assertIn("项目认知入口.md", impacts)
        self.assertIn("系统架构与模块职责.md", impacts)
        self.assertIn("代码符号测试索引与维护规则.md", impacts)
        self.assertNotIn("关键业务不变量与现场约束.md", impacts)

    def test_unknown_new_source_routes_to_generic_project_documents(self) -> None:
        """未来新增的源码模块即使尚未登记名称，也不能绕过文档同步。"""

        impacts = MODULE.impacted_documents(
            ["src/shortagesignalreceiver.cpp", "src/shortagesignalreceiver.h"],
            {
                "src/shortagesignalreceiver.cpp",
                "src/shortagesignalreceiver.h",
            },
        )

        self.assertIn("项目认知入口.md", impacts)
        self.assertIn("系统架构与模块职责.md", impacts)
        self.assertIn("代码符号测试索引与维护规则.md", impacts)
        self.assertIn("关键业务不变量与现场约束.md", impacts)


if __name__ == "__main__":
    unittest.main()
