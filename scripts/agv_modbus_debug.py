# -*- coding: utf-8 -*-
"""仙工 AGV Modbus TCP 联调脚本（零依赖，仅标准库）

寄存器映射来自仙工官方 Modbus 寄存器表，文档地址位以 00001 为起始，
本脚本发送请求时统一减 1 转为 0 基 PDU 地址（与 AgvController::pdu() 一致）。

用法:
    python agv_modbus_debug.py <AGV_IP> [port] <命令> [参数]

命令:
    selfcheck        读 [3x]00070-00073（AGV 自身 IP 四段），验证地址减 1 偏移是否正确
    status           读取并解码：导航站点/定位状态/导航状态/电池/控制权/当前所在站点
    send <站点id>    写 [4x]00001 派单，并以 300ms 轮询导航状态直到 到达/失败/超时
    cancel           写 [0x]00006=1 取消当前导航
    pause            写 [0x]00004=1 暂停导航
    resume           写 [0x]00005=1 继续导航
    confirmloc       写 [0x]00003=1 确认定位正确（仅当定位状态==3 时有效）
    watch            每秒打印一次 status，Ctrl+C 退出

示例:
    python agv_modbus_debug.py 192.168.192.5 selfcheck
    python agv_modbus_debug.py 192.168.192.5 send 1
"""

import socket
import struct
import sys
import time

# 文档地址位（1 基），发请求时减 1
REG_TARGET_STATION = 1   # [4x] 目标站点
REG_NAV_STATION    = 7   # [3x] 当前导航站点
REG_LOC_STATUS     = 8   # [3x] 定位状态
REG_NAV_STATUS     = 9   # [3x] 导航状态
REG_BATTERY        = 13  # [3x] 电池电量
REG_CUR_STATION    = 34  # [3x] 当前所在站点（严格判定 <50cm）
REG_CTRL_SEIZED    = 43  # [3x] 控制权是否被外部抢占
REG_IP_FIRST       = 70  # [3x] 00070-00073 AGV 自身 IP 四段

COIL_CONFIRM_LOC = 3  # [0x] 确认定位正确
COIL_PAUSE_NAV   = 4  # [0x] 暂停导航
COIL_RESUME_NAV  = 5  # [0x] 继续导航
COIL_CANCEL_NAV  = 6  # [0x] 取消导航

NAV_STATUS_TEXT = {
    0: "无导航任务", 1: "等待执行", 2: "正在执行", 3: "暂停",
    4: "到达", 5: "失败", 6: "取消", 7: "超时",
}
LOC_STATUS_TEXT = {0: "定位失败", 1: "定位正确", 2: "正在重定位", 3: "定位完成(需确认)"}


def pdu(doc_addr):
    return doc_addr - 1


class ModbusTcp:
    def __init__(self, ip, port=502, timeout=3.0):
        self.sock = socket.create_connection((ip, port), timeout=timeout)
        self.tid = 0

    def close(self):
        self.sock.close()

    def _request(self, pdu_bytes):
        self.tid = (self.tid + 1) & 0xFFFF
        mbap = struct.pack(">HHHB", self.tid, 0, len(pdu_bytes) + 1, 1)
        self.sock.sendall(mbap + pdu_bytes)
        header = self._recv_exact(7)
        _, _, length, _ = struct.unpack(">HHHB", header)
        body = self._recv_exact(length - 1)
        if body[0] & 0x80:
            raise IOError("Modbus 异常响应: 功能码=0x%02X 异常码=%d" % (body[0] & 0x7F, body[1]))
        return body

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise IOError("连接被对端关闭")
            buf += chunk
        return buf

    def read_input_registers(self, doc_addr, count):
        body = self._request(struct.pack(">BHH", 0x04, pdu(doc_addr), count))
        return list(struct.unpack(">%dH" % count, body[2:]))

    def write_register(self, doc_addr, value):
        self._request(struct.pack(">BHH", 0x06, pdu(doc_addr), value))

    def write_coil(self, doc_addr, on=True):
        self._request(struct.pack(">BHH", 0x05, pdu(doc_addr), 0xFF00 if on else 0x0000))


def to_int16(v):
    return v - 0x10000 if v >= 0x8000 else v


def cmd_selfcheck(mb, agv_ip):
    regs = mb.read_input_registers(REG_IP_FIRST, 4)
    read_ip = ".".join(str(r) for r in regs)
    print("[3x]00070-00073 读到的 AGV 自身 IP: %s" % read_ip)
    print("连接使用的目标 IP:               %s" % agv_ip)
    if read_ip == agv_ip:
        print("结果: 一致，地址减 1 偏移正确")
    else:
        print("结果: 不一致！地址偏移可能有误（试着不减 1 或多减 1 再读一次对比）")


def cmd_status(mb):
    nav = mb.read_input_registers(REG_NAV_STATION, 3)  # 7,8,9
    battery = mb.read_input_registers(REG_BATTERY, 1)[0]
    cur_station = to_int16(mb.read_input_registers(REG_CUR_STATION, 1)[0])
    seized = mb.read_input_registers(REG_CTRL_SEIZED, 1)[0]
    print("当前导航站点 [3x]00007 : %d" % to_int16(nav[0]))
    print("定位状态     [3x]00008 : %d (%s)" % (nav[1], LOC_STATUS_TEXT.get(nav[1], "未知")))
    print("导航状态     [3x]00009 : %d (%s)" % (nav[2], NAV_STATUS_TEXT.get(nav[2], "未知")))
    print("电池电量     [3x]00013 : %d%%" % battery)
    print("当前所在站点 [3x]00034 : %d (0=不在任何站点)" % cur_station)
    print("控制权被抢占 [3x]00043 : %d (%s)" % (seized, "被外部抢占，Modbus 指令可能无效" if seized else "正常"))


def cmd_send(mb, station):
    print("写 [4x]00001 = %d，派单..." % station)
    mb.write_register(REG_TARGET_STATION, station)
    print("开始轮询导航状态（300ms 周期，120s 超时）")
    deadline = time.time() + 120
    last = None
    while time.time() < deadline:
        nav = mb.read_input_registers(REG_NAV_STATION, 3)
        nav_station, nav_status = to_int16(nav[0]), nav[2]
        state = (nav_station, nav_status)
        if state != last:
            print("  导航站点=%d 状态=%d (%s)" % (nav_station, nav_status,
                                                  NAV_STATUS_TEXT.get(nav_status, "未知")))
            last = state
        if nav_status == 4 and nav_station == station:
            print("到达站点 %d" % station)
            return
        if nav_status in (5, 6, 7):
            print("导航异常结束: %s" % NAV_STATUS_TEXT.get(nav_status))
            return
        time.sleep(0.3)
    print("轮询超时（120s），AGV 仍未到达")


def cmd_watch(mb):
    print("每秒刷新，Ctrl+C 退出")
    try:
        while True:
            nav = mb.read_input_registers(REG_NAV_STATION, 3)
            print("导航站点=%d 定位=%s 导航=%s" % (
                to_int16(nav[0]),
                LOC_STATUS_TEXT.get(nav[1], str(nav[1])),
                NAV_STATUS_TEXT.get(nav[2], str(nav[2]))))
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n退出")


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)

    ip = args[0]
    if args[1].isdigit() and len(args) >= 3 and not args[2].isdigit():
        port, cmd_args = int(args[1]), args[2:]
    else:
        port, cmd_args = 502, args[1:]
    cmd = cmd_args[0]

    mb = ModbusTcp(ip, port)
    try:
        if cmd == "selfcheck":
            cmd_selfcheck(mb, ip)
        elif cmd == "status":
            cmd_status(mb)
        elif cmd == "send":
            cmd_send(mb, int(cmd_args[1]))
        elif cmd == "cancel":
            mb.write_coil(COIL_CANCEL_NAV)
            print("已写 [0x]00006=1 取消导航")
        elif cmd == "pause":
            mb.write_coil(COIL_PAUSE_NAV)
            print("已写 [0x]00004=1 暂停导航")
        elif cmd == "resume":
            mb.write_coil(COIL_RESUME_NAV)
            print("已写 [0x]00005=1 继续导航")
        elif cmd == "confirmloc":
            mb.write_coil(COIL_CONFIRM_LOC)
            print("已写 [0x]00003=1 确认定位正确（仅定位状态==3 时有效）")
        elif cmd == "watch":
            cmd_watch(mb)
        else:
            print("未知命令: %s" % cmd)
            print(__doc__)
            sys.exit(1)
    finally:
        mb.close()


if __name__ == "__main__":
    main()
