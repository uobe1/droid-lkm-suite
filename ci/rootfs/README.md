# Rootfs inputs

Planned CI rootfs images:

- Debian 13 arm64 + Docker 29.x (cgroup v1 and cgroup v2 test kernels).
- NixOS stable rootfs for tmpfs xattr/ACL workloads.
- Debian 13 with UFW and Fail2ban installed from pinned packages.
- Droidspaces upstream checkout for the chroot workload.

No image is in use yet and there is no `ci/manifest.json` in this repository.
When an image enters use, pin it by digest in the workflow that consumes it and
record the digest in the gate evidence; floating `latest` is not accepted for
release evidence. No rootfs is committed to this repository.
