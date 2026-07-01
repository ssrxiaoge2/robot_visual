from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]

RZ_JUMP_MARKER = "\u7591\u4f3c Rz \u5927\u89d2\u5ea6\u8df3\u53d8"
SCAN_RETURN_MARKER = (
    "\u626b\u7801\u641c\u7d22\u6210\u529f\uff0c\u56de\u5230\u539f\u5939\u53d6\u4f4d\u7f6e"
)
CHANGELOG_RZ_MARKER = "Rz \u8df3\u53d8\u786e\u8ba4"
README_SCAN_MARKER = "Y \u8f74\u641c\u7801"


def read_text(relative_path: str) -> str:
    try:
        return (ROOT / relative_path).read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"unable to read {relative_path}: {exc}") from exc
    except UnicodeDecodeError as exc:
        raise RuntimeError(f"unable to decode {relative_path} as utf-8: {exc}") from exc


def main() -> int:
    try:
        huayan_h = read_text("src/huayanScheduler.h")
        huayan_cpp = read_text("src/huayanScheduler.cpp")
        task_h = read_text("src/taskexecutor.h")
        task_cpp = read_text("src/taskexecutor.cpp")
        changelog = read_text("changelog/CHANGELOG.md")
        readme = read_text("README.md")
    except RuntimeError as exc:
        print(f"FAILED rz-confirm/scan-search checks: {exc}")
        return 1

    checks = [
        (
            "missing Rz two-frame confirmation markers in huayanScheduler",
            "m_pendingLargeRzConfirmation" in huayan_h
            and "isLargeRzJump" in huayan_cpp
            and RZ_JUMP_MARKER in huayan_cpp,
        ),
        (
            "missing pre-grip scan search API in huayanScheduler.h",
            "movePreGripScanSearchTo" in huayan_h
            and "preGripScanSearchMoveCompleted" in huayan_h
            and "ReturnToCaptureForScanFailure" in huayan_h,
        ),
        (
            "missing scan search state-machine markers in taskexecutor",
            "PreGripScanSearchMove" in task_h
            and "PreGripScanSearchReturn" in task_h
            and "PreGripScanReturnCapture" in task_h
            and "startNextScanSearchMove" in task_cpp
            and "finishScanSearchFailureAtCapture" in task_cpp,
        ),
        (
            "missing scan-search Y offset and success-return markers in taskexecutor",
            "kScanSearchYOffsetMm" in task_h
            and "20.0" in task_h
            and "movePreGripScanSearchTo(0.0)" in task_cpp
            and SCAN_RETURN_MARKER in task_cpp,
        ),
        (
            "missing v0.2.2 Rz documentation markers",
            "v0.2.2" in changelog
            and CHANGELOG_RZ_MARKER in changelog
            and README_SCAN_MARKER in readme,
        ),
    ]

    failed = [message for message, ok in checks if not ok]
    if failed:
        print("FAILED rz-confirm/scan-search checks:")
        for item in failed:
            print(item)
        return 1

    print("rz-confirm/scan-search checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
