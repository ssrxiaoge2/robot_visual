#!/usr/bin/env python3
import sys
import time

try:
    from pyorbbecsdk import Context, Config, Pipeline, OBSensorType, OBError
except ImportError as e:
    print(f"IMPORT_ERROR: {e}", file=sys.stderr)
    sys.exit(1)


def main():
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.10"
    ctx = Context()

    device_list = ctx.query_devices()
    if device_list.get_count() == 0:
        print(f"没有检测到奥比相机设备 (IP={ip})", file=sys.stderr)
        sys.exit(1)

    name = device_list.get_device_name_by_index(0)
    sn = device_list.get_device_serial_number_by_index(0)
    dev_ip = device_list.get_device_ip_address_by_index(0)
    print(f"设备: {name}  SN: {sn}  IP: {dev_ip}")

    device = device_list.get_device_by_index(0)
    pipeline = Pipeline(device)

    for sensor_type in [OBSensorType.DEPTH_SENSOR, OBSensorType.COLOR_SENSOR]:
        profiles = pipeline.get_stream_profile_list(sensor_type)
        if profiles.get_count() > 0:
            config = Config()
            config.enable_stream(profiles[0])
            pipeline.start(config)
            time.sleep(0.3)
            pipeline.stop()
            print("OK")
            return

    print("没有可用的视频流", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    try:
        main()
    except OBError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
