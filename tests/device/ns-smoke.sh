#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
echo "child_pid=$$"
echo "child_pidns=$(readlink /proc/self/ns/pid 2>/dev/null)"
echo "child_pid_for_children=$(readlink /proc/self/ns/pid_for_children 2>/dev/null)"
echo "child_ipcns=$(readlink /proc/self/ns/ipc 2>/dev/null)"
echo "proc1=$(head -1 /proc/1/status 2>/dev/null)"
echo "proc_count=$(ls /proc 2>/dev/null | grep -c '^[0-9][0-9]*$')"
