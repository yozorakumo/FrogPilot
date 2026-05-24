#!/usr/bin/env python3
"""Test V4L2 HW decoder on C3 - debug format enum"""
import fcntl
import struct
import os

VIDIOC_QUERYCAP = 0x80685600
VIDIOC_ENUM_FMT = 0xc0405602

V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE = 10
V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE = 9

DECODER_DEVICE = "/dev/v4l/by-path/platform-aa00000.qcom_vidc-video-index0"

fd = os.open(DECODER_DEVICE, os.O_RDWR | os.O_NONBLOCK)
print(f"FD: {fd}")

for buf_type in [V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE]:
    type_name = "OUTPUT" if buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE else "CAPTURE"
    print(f"\n=== {type_name} formats ===")
    for i in range(10):
        # v4l2_fmtdesc: index(4) + type(4) + flags(4) + description(32) + pixelformat(4) + reserved(16) = 64
        buf = bytearray(64)
        struct.pack_into('<I', buf, 0, i)      # index
        struct.pack_into('<I', buf, 4, buf_type) # type
        try:
            fcntl.ioctl(fd, VIDIOC_ENUM_FMT, buf)
            # Dump raw hex for debugging
            flags = struct.unpack_from('<I', buf, 8)[0]
            description = buf[12:44].split(b'\x00')[0].decode('ascii', errors='replace')
            pixelformat = struct.unpack_from('<I', buf, 44)[0]
            fmt_chars = ''.join(chr((pixelformat >> (8*j)) & 0xFF) for j in range(4))
            print(f"  [{i}] raw bytes 0-63: {buf.hex()}")
            print(f"       flags=0x{flags:x} desc='{description}' pixelformat=0x{pixelformat:08x} chars='{fmt_chars}'")
        except OSError as e:
            print(f"  [{i}] Error: {e}")
            break

os.close(fd)