from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    header = read_text("src/mainwindow.h")
    source = read_text("src/mainwindow.cpp")

    checks = {
        "source toggle controls exist":
            "QButtonGroup" in header
            and "m_shortageMockRadio" in header
            and "m_shortageLiveRadio" in header
            and 'QRadioButton(QStringLiteral("模拟缺料"))' in source
            and 'QRadioButton(QStringLiteral("现场系统"))' in source,
        "live mode start and stop are wired to monitoring":
            "startShortageMonitoring();" in source
            and "stopShortageMonitoring();" in source,
        "switching back to mock mode stops live monitoring":
            "void MainWindow::onShortageSourceChanged()" in source
            and "m_devMgr->stopShortageMonitoring();" in source,
        "queue table shows task source column":
            "taskSourceText(task.source)" in source
            and 'QStringLiteral("来源")' in source,
        "live status panel shows product mode and bits":
            "m_shortageProductLabel" in header
            and "m_shortageModeLabel" in header
            and "m_shortageBitsLabel" in header
            and 'form->addRow(QStringLiteral("产品:")' in source
            and 'form->addRow(QStringLiteral("方式:")' in source
            and 'form->addRow(QStringLiteral("L 位:")' in source,
        "live status table exists for 12 stations":
            "m_shortageStateTable" in header
            and "new QTableWidget(12, 5, group)" in source,
        "live mode disables manual mock station buttons":
            "stationButtonsEnabled = !liveMode" in source,
    }

    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        print("FAILED shortage source UI checks:")
        for name in failed:
            print(f"- {name}")
        return 1

    print("shortage source UI checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
