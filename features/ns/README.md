# dnsk_ns (M1 development)

Vendored Droidspaces-LKM 6.12 PID/IPC namespace implementation.

- Build target: `android16-6.12` only for now.
- Loader: `dnskctl load ns` passes `gki=1 activate=1`.
- Status: development; no release/tag claim.
- Core status bridge is optional: with core loaded first, `/proc/dnsk/status` shows the
  `ns` feature row; without core the namespace implementation still works.
- `activate=0` loads and unloads cleanly; `tests/device/core-bridge-smoke.sh` verifies
  the bridge and the core pin/unpin order.
- Unload is safe: activation no longer pins the module.  `ds_port_exit` first
  quiesces (`ds_ns_quiesce()`: block every IPC/PID entry, take an RCU grace,
  then wait the `ds_ns_inflight` counter to zero) and only then restores the
  hooks and frees code.  Every sleep-capable entry takes a `try_module_get()`
  reference, so a bare `rmmod` answers `EAGAIN` while a caller is still inside
  module text -- `stop_machine()`/`synchronize_rcu()` alone cannot see a task
  parked in a blocking SYSVIPC/mqueue syscall or the `do_exit` body.  Leftover
  per-task IPC state is inert heap and is reported, not WARNed, on unload.

Pre-existing tasks and pidfd setns: with `CONFIG_SYSVIPC=n` the kernel's
`init_nsproxy.ipc_ns` is NULL by construction (its initializer is guarded by
`CONFIG_SYSVIPC`/`CONFIG_POSIX_MQUEUE`) and the stock `copy_ipcs()` stub
propagates that NULL, so a task created before the module has nothing to
join and pidfd `setns(CLONE_NEWIPC)` fails closed (EINVAL).  Such a task is
not in some other IPC namespace - the kernel cannot create one - so its real
namespace is the backend's initial namespace, the same object
`copy_ipcs()`/`ds_current_ipc_ns()` already fall back to.  On activation the
module installs that namespace into the kernel's own `init_nsproxy` and into
every nsproxy that exists at load time (RCU walk, one `task_lock`-protected
reference per nsproxy).  Unload raises `ds_ipc_quiescing` while the hooks are
still installed, clears those pointers and drops their references, then
removes the hooks and destroys the backend, so a task created during teardown
never picks up a reference the teardown walk cannot see.  `validate_nsset()`
keeps failing closed on a NULL `ipc_ns` as a guard.  The walk iterates task
group leaders; threads share the leader's nsproxy on this kernel, unless one
individually switched a *different* namespace type before the module loaded
(the IPC namespace itself cannot exist yet), in which case that thread stays
fail-closed.
