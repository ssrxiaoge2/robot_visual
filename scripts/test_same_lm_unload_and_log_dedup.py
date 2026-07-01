from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def read_text(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def main() -> int:
    task_cpp = read_text("src/taskexecutor.cpp")
    huayan_h = read_text("src/huayanScheduler.h")
    huayan_cpp = read_text("src/huayanScheduler.cpp")
    mainwindow_cpp = read_text("src/mainwindow.cpp")
    changelog = read_text("changelog/CHANGELOG.md")

    arm_pickup_block = re.search(
        r"case ExecState::ArmPickup:(.*?)case ExecState::StowAfterPickup:",
        task_cpp,
        re.S,
    )
    arm_pickup_text = arm_pickup_block.group(1) if arm_pickup_block else ""

    checks = [
        (
            "same-LM pickup completion must start unload after capture safety height",
            "pickupLm == m_stationCfg->unloadLm" in arm_pickup_text
            and "enterState(ExecState::ArmUnload" in arm_pickup_text
            and "StowAfterPickup" not in arm_pickup_text,
        ),
        (
            "TaskExecutor must not relay raw arm logs with task prefix",
            "HuayanScheduler::logMessage" not in task_cpp
            and 'prefix(QStringLiteral("ARM")) + QStringLiteral(" ") + message'
            not in task_cpp,
        ),
        (
            "MainWindow must not directly connect HuayanScheduler logMessage",
            "connect(hs, &HuayanScheduler::logMessage" not in mainwindow_cpp,
        ),
        (
            "normal stage completion must stop silently",
            "void stop(bool emitStoppedLog = true)" in huayan_h
            and "stop(false)" in huayan_cpp
            and "emitStoppedLog" in huayan_cpp,
        ),
        (
            "changelog records v0.2.3 same-LM unload and log dedup",
            "v0.2.3" in changelog
            and "同站工位" in changelog
            and "日志去重" in changelog,
        ),
    ]

    failed = [message for message, ok in checks if not ok]
    if failed:
        print("FAILED same-lm/log-dedup checks:")
        for item in failed:
            print(item)
        return 1

    print("same-lm/log-dedup checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
