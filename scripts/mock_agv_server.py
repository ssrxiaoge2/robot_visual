# -*- coding: utf-8 -*-
"""本地模拟仙工 AGV Modbus TCP 服务，用于无真机时测试联调脚本和 Qt 上位机。

模拟行为：
    [4x]00001 写入站点 id 后，导航状态 [3x]00009 依次变为 2（执行中，持续 2 秒）→ 4（到达），
    同时 [3x]00007 导航站点、[3x]00034 当前所在站点 跟随更新
    [0x]00006 写 1 取消导航，状态变 6
    [0x]00004 / 00005 暂停 / 继续导航（状态 3 / 2）
    [3x]00070-00073 返回 127.0.0.1（配合联调脚本 selfcheck 命令）
    [3x]00013 电池电量固定 88

用法:
    python mock_agv_server.py [port]      # 默认端口 1502（502 需管理员权限）

配套测试:
    python agv_modbus_debug.py 127.0.0.1 1502 selfcheck
    python agv_modbus_debug.py 127.0.0.1 1502 send 3
    Qt 上位机：AGV IP 填 127.0.0.1，端口 1502
"""

import socket
import struct
import sys
import threading
import time

NAV_SECONDS = 2  # 模拟行走耗时

input_regs = {}  # 0 基 PDU 地址 -> 值
for i, part in enumerate([127, 0, 0, 1]):
    input_regs[69 + i] = part  # [3x]00070-00073 自身 IP
input_regs[6] = 0    # [3x]00007 导航站点
input_regs[7] = 1    # [3x]00008 定位状态=正确
input_regs[8] = 0    # [3x]00009 导航状态=无
input_regs[12] = 88  # [3x]00013 电池电量
input_regs[33] = 0   # [3x]00034 当前所在站点
input_regs[42] = 0   # [3x]00043 控制权未被抢占

nav_lock = threading.Lock()
nav_seq = 0  # 派单序号，新单可打断旧单


def log(msg):
    print("[mock-agv] %s" % msg, flush=True)


def start_nav(station):
    global nav_seq
    with nav_lock:
        nav_seq += 1
        seq = nav_seq

    def run():
        with nav_lock:
            input_regs[6] = station
            input_regs[8] = 2
            input_regs[33] = 0
        log("收到派单：站点 %d，开始行走（%ds）" % (station, NAV_SECONDS))
        deadline = time.time() + NAV_SECONDS
        while time.time() < deadline:
            time.sleep(0.1)
            with nav_lock:
                if nav_seq != seq or input_regs[8] in (3, 6):  # 被新单打断/暂停/取消
                    if input_regs[8] == 3:
                        continue  # 暂停中等待继续
                    return
        with nav_lock:
            if nav_seq != seq:
                return
            input_regs[8] = 4
            input_regs[33] = station
        log("到达站点 %d" % station)

    threading.Thread(target=run, daemon=True).start()


def handle_pdu(pdu):
    fc = pdu[0]
    if fc == 4:  # 读输入寄存器
        addr, count = struct.unpack(">HH", pdu[1:5])
        with nav_lock:
            vals = [input_regs.get(addr + i, 0) for i in range(count)]
        return struct.pack(">BB", 4, count * 2) + struct.pack(">%dH" % count, *vals)
    if fc == 6:  # 写单个保持寄存器
        addr, value = struct.unpack(">HH", pdu[1:5])
        if addr == 0 and value > 0:  # [4x]00001 目标站点
            start_nav(value)
        else:
            log("写保持寄存器 pdu=%d 值=%d（未模拟，忽略）" % (addr, value))
        return pdu
    if fc == 5:  # 写单个线圈
        addr, value = struct.unpack(">HH", pdu[1:5])
        if value == 0xFF00:
            with nav_lock:
                if addr == 5:    # [0x]00006 取消
                    input_regs[8] = 6
                    log("导航已取消")
                elif addr == 3:  # [0x]00004 暂停
                    if input_regs[8] == 2:
                        input_regs[8] = 3
                        log("导航已暂停")
                elif addr == 4:  # [0x]00005 继续
                    if input_regs[8] == 3:
                        input_regs[8] = 2
                        log("导航已继续")
                elif addr == 2:  # [0x]00003 确认定位
                    if input_regs[7] == 3:
                        input_regs[7] = 1
                        log("定位已确认")
                else:
                    log("写线圈 pdu=%d（未模拟，忽略）" % addr)
        return pdu
    return struct.pack(">BB", fc | 0x80, 1)  # 非法功能码


def handle_client(conn, peer):
    log("客户端接入: %s:%d" % peer)
    try:
        while True:
            header = conn.recv(7)
            if len(header) < 7:
                return
            tid, _, length, unit = struct.unpack(">HHHB", header)
            pdu = conn.recv(length - 1)
            body = handle_pdu(pdu)
            conn.sendall(struct.pack(">HHHB", tid, 0, len(body) + 1, unit) + body)
    except (ConnectionError, OSError):
        pass
    finally:
        conn.close()
        log("客户端断开: %s:%d" % peer)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 1502
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    log("模拟 AGV 已启动: 127.0.0.1:%d（Ctrl+C 退出）" % port)
    try:
        while True:
            conn, peer = srv.accept()
            threading.Thread(target=handle_client, args=(conn, peer), daemon=True).start()
    except KeyboardInterrupt:
        log("退出")


if __name__ == "__main__":
    main()
