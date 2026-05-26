#!/usr/bin/env python3
import sys
import signal
import os

try:
    from pyorbbecsdk import Context, Pipeline, Config, OBSensorType, OBFormat
except:
    pass

running = True

def on_signal(sig, frame):
    global running
    running = False

signal.signal(signal.SIGTERM, on_signal)
signal.signal(signal.SIGINT, on_signal)


def main():
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.10"
    ctx = Context()
    dev = ctx.create_net_device(ip, 8090)
    if not dev:
        os.write(2, b"CANNOT_CONNECT\n")
        sys.exit(1)

    pipeline = Pipeline(dev)
    config = Config()

    profiles = pipeline.get_stream_profile_list(OBSensorType.COLOR_SENSOR)
    profile = profiles.get_video_stream_profile(640, 360, OBFormat.MJPG, 10)
    if not profile:
        profile = profiles.get_default_video_stream_profile()
    config.enable_stream(profile)

    pipeline.start(config)
    os.write(2, b"OK\n")

    try:
        while running:
            frames = pipeline.wait_for_frames(1000)
            if frames:
                color_frame = frames.get_color_frame()
                if color_frame:
                    data = color_frame.get_data().tobytes()
                    if data:
                        header = str(len(data)).encode() + b'\n'
                        os.write(1, header)
                        os.write(1, data)
    finally:
        pipeline.stop()


if __name__ == "__main__":
    main()
