# beryl device kernel config

`beryl-6.12.30.config.gz` is the exact config copied from the device
`/proc/config.gz` on 2026-11-06 (kernel
`6.12.30-android16-5-g1ed949324a3e-ab13881345-4k`, Redmi Note 14 5G).

It is the base for QEMU/DKK oracle builds: copy it over the build `.config`,
then apply only the explicit test deltas in `.github/workflows/qemu-core.yml`
(namespaces, POSIX mqueue, netfilter/ipset, devtmpfs for the test rootfs).
Do not hand-tune controller counts or layouts; the device config is the
authority. Forks can replace this file with their own device config.

The device `module_layout` modversion CRC observed from a vendor module is
`0xe976b219`; `device-image.yml` patches it into test modules so the device
kernel can load them without KernelSU's loader.
