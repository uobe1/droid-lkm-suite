#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal SysV message-queue smoke using the Android libc."""
import ctypes
import os

libc = ctypes.CDLL(None, use_errno=True)
IPC_PRIVATE = 0
IPC_CREAT = 0o1000
IPC_RMID = 0


class Msgbuf(ctypes.Structure):
    _fields_ = [("mtype", ctypes.c_long), ("mtext", ctypes.c_char * 32)]


def errno():
    return ctypes.get_errno()


def main():
    print("pid", os.getpid(), "ipcns", os.readlink("/proc/self/ns/ipc"))
    q = libc.msgget(IPC_PRIVATE, IPC_CREAT | 0o600)
    print("msgget", q, "errno", errno())
    if q < 0:
        return 1
    msg = Msgbuf()
    msg.mtype = 1
    msg.mtext = b"hello"
    rc = libc.msgsnd(q, ctypes.byref(msg), 5, 0)
    print("msgsnd", rc, "errno", errno())
    out = Msgbuf()
    rc = libc.msgrcv(q, ctypes.byref(out), 32, 1, 0)
    data = bytes(out.mtext[:rc]) if rc > 0 else b""
    print("msgrcv", rc, data, "errno", errno())
    rc = libc.msgctl(q, IPC_RMID, None)
    print("msgctl_rmid", rc, "errno", errno())
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
