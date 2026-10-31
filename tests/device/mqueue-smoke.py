#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal POSIX mqueue smoke using the Android libc."""
import ctypes
import os

libc = ctypes.CDLL(None, use_errno=True)
O_CREAT = 0o100
O_RDWR = 0o2
O_EXCL = 0o200
name = f"dnsk-smoke-{os.getpid()}"
name_b = name.encode()
# The raw syscall ABI takes the name without a leading slash; libc mq_open
# wrappers strip it before entering the kernel.

# arm64 __NR_mq_open=180, mq_unlink=181, mq_timedsend=182, mq_timedreceive=183.
# Bionic does not export the mq_* wrappers, so use the raw syscall interface.
NR_MQ_OPEN = 180
NR_MQ_UNLINK = 181
NR_MQ_SEND = 182
NR_MQ_RECEIVE = 183
libc.syscall.restype = ctypes.c_long


def sys_mq_open(name_bytes, flags, mode):
    return libc.syscall(NR_MQ_OPEN, name_bytes, flags, mode, None)


def sys_mq_send(fd, data, prio):
    buf = ctypes.create_string_buffer(data)
    return libc.syscall(NR_MQ_SEND, fd, buf, len(data), prio, None)


def sys_mq_receive(fd, size):
    buf = ctypes.create_string_buffer(size)
    prio = ctypes.c_uint(0)
    rc = libc.syscall(NR_MQ_RECEIVE, fd, buf, size, ctypes.byref(prio), None)
    return rc, buf.value, prio.value


def sys_mq_unlink(name_bytes):
    return libc.syscall(NR_MQ_UNLINK, name_bytes)


def errno():
    return ctypes.get_errno()


def main():
    print("pid", os.getpid(), "ipcns", os.readlink("/proc/self/ns/ipc"))
    fd = sys_mq_open(name_b, O_CREAT | O_RDWR | O_EXCL, 0o600)
    print("mq_open", fd, "errno", errno())
    if fd < 0:
        return 1
    rc = sys_mq_send(fd, b"hello", 1)
    print("mq_send", rc, "errno", errno())
    # The receive buffer must be at least the queue's mq_msgsize (default 8192).
    rc, data, prio = sys_mq_receive(fd, 8192)
    print("mq_receive", rc, data, "prio", prio, "errno", errno())
    print("mq_close", os.close(fd), "errno", errno())
    print("mq_unlink", sys_mq_unlink(name_b), "errno", errno())
    return 0 if rc > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
