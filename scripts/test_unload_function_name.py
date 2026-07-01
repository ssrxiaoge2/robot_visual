from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
lineconfig = (ROOT / "src" / "lineconfig.h").read_text(encoding="utf-8")
readme = (ROOT / "README.md").read_text(encoding="utf-8")
changelog = (ROOT / "changelog" / "CHANGELOG.md").read_text(encoding="utf-8")


checks = [
    (
        "station config uses Func_fanzhuan",
        "Func_fanzhuan" in lineconfig and "FlipUnload" not in lineconfig,
    ),
    (
        "README uses Func_fanzhuan for unload function docs",
        "Func_fanzhuan" in readme and "FlipUnload" not in readme,
    ),
    (
        "changelog records unload function rename",
        "Func_fanzhuan" in changelog and "倒料函数名" in changelog,
    ),
]


failed = [name for name, ok in checks if not ok]
if failed:
    print("FAILED unload function checks:")
    for item in failed:
        print(f"- {item}")
    sys.exit(1)

print("unload function checks passed")
