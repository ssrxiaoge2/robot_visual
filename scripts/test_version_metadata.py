from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
mainwindow = (ROOT / "src" / "mainwindow.cpp").read_text(encoding="utf-8")
changelog = (ROOT / "changelog" / "CHANGELOG.md").read_text(encoding="utf-8")


checks = [
    (
        "cmake project version matches latest release",
        "project(wh-robot-visual VERSION 0.2.5 LANGUAGES CXX)" in cmake
        and "## 2026-07-02 | v0.2.5 |" in changelog,
    ),
    (
        "cmake defines APP_VERSION and APP_BUILD_DATE",
        'APP_VERSION="${PROJECT_VERSION}"' in cmake
        and 'APP_BUILD_DATE="${APP_BUILD_DATE}"' in cmake,
    ),
    (
        "mainwindow shows version in title",
        'setWindowTitle(QStringLiteral("仓储机器人可视化上位机 v%1").arg(QStringLiteral(APP_VERSION)))'
        in mainwindow,
    ),
    (
        "mainwindow logs startup version and build date",
        "APP_VERSION" in mainwindow
        and "APP_BUILD_DATE" in mainwindow
        and "上位机可视化系统启动 v%1（Build %2）" in mainwindow,
    ),
    (
        "changelog records version feature entry",
        "客户现场缺料信号实施阶段（开发版）" in changelog
        and "v0.2.5" in changelog,
    ),
]


failed = [name for name, ok in checks if not ok]
if failed:
    print("FAILED version metadata checks:")
    for item in failed:
        print(f"- {item}")
    sys.exit(1)

print("version metadata checks passed")
