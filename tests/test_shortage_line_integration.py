from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    header = read_text("src/devicemanager.h")
    source = read_text("src/devicemanager.cpp")
    line_header = read_text("src/linemanager.h")
    line_source = read_text("src/linemanager.cpp")

    checks = {
        "DeviceManager exposes shortage monitor getter":
            "ShortageMonitor *shortageMonitor() const" in header,
        "DeviceManager exposes live monitoring controls":
            "void startShortageMonitoring();" in header
            and "void stopShortageMonitoring();" in header
            and "void DeviceManager::startShortageMonitoring()" in source
            and "void DeviceManager::stopShortageMonitoring()" in source,
        "DeviceManager creates ShortageMonitor":
            "m_shortageMonitor = new ShortageMonitor(m_customSysScheduler, this);" in source,
        "DeviceManager forwards shortageRequested into line FIFO":
            "connect(m_shortageMonitor, &ShortageMonitor::shortageRequested," in source
            and "reportShortage(stationId, TaskSource::CustomerSystem)" in source,
        "DeviceManager feeds FIFO acceptance back to monitor":
            "confirmShortageAccepted(stationId, accepted)" in source,
        "LineManager reportShortage keeps source parameter":
            "bool reportShortage(int stationId,\n                       TaskSource source = TaskSource::UiMock);" in line_header
            or "bool reportShortage(int stationId, TaskSource source = TaskSource::UiMock);" in line_header,
        "LineManager forwards task source into queue":
            "m_queue.enqueue(stationId, source);" in line_source,
        "LineManager rejects shortage reports while in Error":
            "if (m_state == LineSystemState::Error)" in line_source,
    }

    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        print("FAILED shortage line integration checks:")
        for name in failed:
            print(f"- {name}")
        return 1

    print("shortage line integration checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
