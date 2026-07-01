from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    header = read_text("src/taskexecutor.h")
    source = read_text("src/taskexecutor.cpp")
    cmake = read_text("CMakeLists.txt")
    mainwindow = read_text("src/mainwindow.cpp")

    checks = {
        "pickup/unload same-LM special case removed":
            "pickupLm == unloadLm" not in source
            and "取料位与倒料位同为 LM%1" not in source,
        "already-at-target AGV shortcut removed":
            "m_lastAgvMonitor.curStation == lm" not in source
            and "completeAgvStepArrival" not in source
            and "AgvMonitorData m_lastAgvMonitor" not in header
            and "bool m_hasAgvMonitor" not in header,
        "startup version log is wired":
            "APP_VERSION" in mainwindow
            and "APP_VERSION" in cmake
            and "APP_BUILD_DATE" in mainwindow
            and "APP_BUILD_DATE" in cmake,
    }

    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        print("FAILED same-LM/version regression checks:")
        for name in failed:
            print(f"- {name}")
        return 1

    print("same-LM/version regression checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
