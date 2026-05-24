#!/usr/bin/env python3
"""Test V4L2 HW decoder on C3 (msm_vidc_vdec) - enumerate supported formats"""
import fcntl
import struct
import os
import sys

# V4L2 ioctl constants
VIDIOC_QUERYCAP = 0x80685600
VIDIOC_ENUM_FMT = 0xc0405602
VIDIOC_S_FMT = 0xc0cc5605
VIDIOC_G_FMT = 0xc0cc5604
VIDIOC_REQBUFS = 0xc0145608
VIDIOC_QBUF = 0xc058560f
VIDIOC_DQBUF = 0xc0585611
VIDIOC_STREAMON = 0x40045612
VIDIOC_STREAMOFF = 0x40045613
VIDIOC_S_CTRL = 0xc008561c
VIDIOC_G_CTRL = 0xc008561b
VIDIOC_DECODER_CMD = 0xc028560e

# V4L2 buffer types
V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE = 9
V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE = 10

# V4L2 memory types
V4L2_MEMORY_USERPTR = 1
V4L2_MEMORY_MMAP = 1

# Pixel formats
def fourcc(a, b, c, d):
    return struct.unpack('<I', bytes([a, b, c, d]))[0]

V4L2_PIX_FMT_NV12 = fourcc(ord('N'), ord('V'), ord('1'), ord('2'))
V4L2_PIX_FMT_HEVC = fourcc(ord('H'), ord('E'), ord('V'), ord('C'))
V4L2_PIX_FMT_H264 = fourcc(ord('H'), ord('2'), ord('6'), ord('4'))

DECODER_DEVICE = "/dev/v4l/by-path/platform-aa00000.qcom_vidc-video-index0"

def query_cap(fd):
    """Query device capabilities"""
    cap = bytearray(104)
    fcntl.ioctl(fd, VIDIOC_QUERYCAP, cap)
    driver = cap[0:16].split(b'\x00')[0].decode('ascii', errors='replace')
    card = cap[16:32].split(b'\x00')[0].decode('ascii', errors='replace')
    bus_info = cap[32:48].split(b'\x00')[0].decode('ascii', errors='replace')
    capabilities = struct.unpack('<I', cap[48:52])[0]
    print(f"Driver: {driver}")
    print(f"Card: {card}")
    print(f"Bus: {bus_info}")
    print(f"Capabilities: 0x{capabilities:08x}")
    return driver, card

def enum_fmts(fd, buf_type):
    """Enumerate supported formats"""
    type_name = "OUTPUT (compressed input)" if buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE else "CAPTURE (raw output)"
    print(f"\n=== {type_name} formats ===")
    fmts = []
    for i in range(30):
        # struct v4l2_fmtdesc: 64 bytes
        buf = struct.pack('<II', i, buf_type) + bytearray(56)
        try:
            fcntl.ioctl(fd, VIDIOC_ENUM_FMT, buf)
            flags = struct.unpack('<I', buf[8:12])[0]
            desc = buf[12:44].split(b'\x00')[0].decode('ascii', errors='replace')
            pixfmt = struct.unpack('<I', buf[44:48])[0]
            fmt_chars = ''.join(chr((pixfmt >> (8*i)) & 0xFF) for i in range(4))
            fmts.append((fmt_chars, desc, flags))
            flag_str = " (compressed)" if flags & 1 else ""
            print(f"  [{i}] {fmt_chars} - {desc}{flag_str} (flags=0x{flags:x})")
        except OSError as e:
            break
    return fmts

def main():
    print(f"Opening decoder device: {DECODER_DEVICE}")
    fd = os.open(DECODER_DEVICE, os.O_RDWR | os.O_NONBLOCK)
    print(f"FD: {fd}")

    driver, card = query_cap(fd)
    assert driver == "msm_vidc_driver", f"Unexpected driver: {driver}"
    assert card == "msm_vidc_vdec", f"Unexpected card: {card}"
    print("\n✓ Confirmed msm_vidc_vdec decoder device")

    # Enumerate formats
    out_fmts = enum_fmts(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
    cap_fmts = enum_fmts(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)

    # Check HEVC support
    hevc_supported = any(f[0] == 'HEVC' for f in out_fmts)
    h264_supported = any(f[0] == 'H264' for f in out_fmts)
    nv12_supported = any(f[0] == 'NV12' for f in cap_fmts)

    print(f"\n=== Summary ===")
    print(f"HEVC decode: {'✓' if hevc_supported else '✗'}")
    print(f"H264 decode: {'✓' if h264_supported else '✗'}")
    print(f"NV12 output: {'✓' if nv12_supported else '✗'}")

    os.close(fd)
    print("\n✓ Device closed successfully")

if __name__ == "__main__":
    main()