# dnsk_ipset origin

## Upstream pin

- Repository: AOSP `kernel/common` (Android common kernel), branch `android16-6.12`
- Tag: `android16-6.12-2025-09_r12` (annotated tag object `15cb176d0e05e89a4d2b2c83f6d8311f48c43979`)
- Commit: `e1b7f2c3c5bdb42b8083b0199de89b1b155e9daf` (2025-11-16, Linux 6.12.38)
- Tag URL: <https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-09_r12>
- File URL template:
  <https://android.googlesource.com/kernel/common/+/e1b7f2c3c5bdb42b8083b0199de89b1b155e9daf/PATH>
- Mirror used for fetching:
  <https://github.com/aosp-mirror/kernel_common/tree/e1b7f2c3c5bdb42b8083b0199de89b1b155e9daf>

This is the newest `android16-6.12` tag available in the AOSP mirror at
implementation time. The target DDK KMI is `android16-6.12`; the build command
uses `/opt/ddk/kdir/android16-6.12`. The device kernel reports 6.12.30 while
this tag is 6.12.38. `net/netfilter/ipset/ip_set_core.c` from this tag is
byte-identical to stable `v6.12.38` from
<https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git>, so the
pinned tree is an unmodified upstream source for the vendored ipset units.

Upstream 6.12 has no `net/netfilter/ipset/ip_set_core.h`; the private core
interface is `include/linux/netfilter/ipset/ip_set.h`. No such file was
invented here.

## Licenses

Vendored files keep their original SPDX headers:

- `net/netfilter/ipset/*.c`, `net/netfilter/ipset/*.h`,
  `net/netfilter/xt_set.c`: `GPL-2.0-only`.
  License text: `LICENSES/GPL-2.0-only.txt`.
- `include/linux/netfilter/ipset/*.h`: `GPL-2.0` / `GPL-2.0-only` as marked
  per file; both are GPL-2.0-only in effect.
- `include/uapi/linux/netfilter/ipset/*.h`,
  `include/uapi/linux/netfilter/xt_set.h`: `GPL-2.0 WITH Linux-syscall-note`.
  The exception text is the kernel's
  `LICENSES/exceptions/Linux-syscall-note` at the pinned commit.

## Vendored paths

From `net/netfilter/ipset/`:

- `ip_set_core.c`
- `ip_set_hash_gen.h`
- `ip_set_hash_ip.c`
- `ip_set_hash_net.c`
- `ip_set_bitmap_gen.h` (vendored for reference; not compiled, no bitmap type
  is registered)
- `pfxlen.c` (required by hash:ip/hash:net for prefix maps; part of upstream
  `ip_set-y`)

From `net/netfilter/`:

- `xt_set.c`

From `include/linux/netfilter/ipset/`:

- `ip_set.h`, `ip_set_bitmap.h`, `ip_set_getport.h`, `ip_set_hash.h`,
  `ip_set_list.h`, `pfxlen.h`

From `include/uapi/linux/netfilter/ipset/`:

- `ip_set.h`, `ip_set_bitmap.h`, `ip_set_hash.h`, `ip_set_list.h`

From `include/uapi/linux/netfilter/`:

- `xt_set.h`

Not vendored: `ip_set_getport.c` and the bitmap/list type sources. The two
supported types do not use the port helpers, and no bitmap/list type is
registered.

## Local changes

Only build integration, no semantic changes:

- `ip_set_core.c`: `ip_set_init()`/`ip_set_fini()` become non-static
  `dnsk_ipset_core_init()`/`dnsk_ipset_core_exit()`; `__init`/`__exit` and
  `module_init()`/`module_exit()` are removed because the unit is linked into
  one module.
- `ip_set_hash_ip.c`, `ip_set_hash_net.c`, `xt_set.c`: same treatment, with
  names `dnsk_ipset_hash_ip_*`, `dnsk_ipset_hash_net_*`,
  `dnsk_ipset_xt_set_*`.
- `Kbuild`/`Makefile` are project-specific; output is `dnsk_ipset.ko`.
- `main.c`/`units.h` are project glue: one module init registers core, both set
  types and xt_set in that order, with reverse rollback in the error path and a
  `registered` guard in module exit.
- The vendored 6.12 headers are placed before the kernel tree includes, so the
  build does not depend on the DDK KMI installing the `CONFIG_IP_SET` headers.
- `CONFIG_IP_SET_MAX=256` is supplied on the command line; the upstream
  `max_sets` module parameter still overrides it at load time.

## Upstream SHA-256 (before local adaptation)

```
0d3f4a091e4d4d69ef6595bf7e6781f1ba1e0397abef322ac953425a16af0d54  include/linux/netfilter/ipset/ip_set.h
772a9b280a8ff22d883bf3b4e8aba9d9e3f2097d64e205dffece622892e2c4ca  include/linux/netfilter/ipset/ip_set_bitmap.h
2d94470ead031062044d41b664d5ad5cacb522ed3a5156f6667f3d8da07bebe7  include/linux/netfilter/ipset/ip_set_getport.h
3a2bb72a7d9908f2b9767101081bcc617f2b756f9315f2eadce8ca6c9ca20bed  include/linux/netfilter/ipset/ip_set_hash.h
855073270f1643a3fc12488f607dfba4d1a3bbd5bb4ca2f0201eb8003462761f  include/linux/netfilter/ipset/ip_set_list.h
17b2989a879235b8499fd07fd6180fa41002107b988baebc0455ea1c39d1c737  include/linux/netfilter/ipset/pfxlen.h
1480ddb2702d043824eb8dd2e4df813854995cccf9694bfd35222aa4c186c38a  include/uapi/linux/netfilter/ipset/ip_set.h
14644df12ee167e2b3a47303e3004798952e9c3228f046438ac272f6d2f58c1b  include/uapi/linux/netfilter/ipset/ip_set_bitmap.h
3e3231492a1ea94c5012b978cf6335a65306d5a92e0a8ad62318783df61313c4  include/uapi/linux/netfilter/ipset/ip_set_hash.h
dbad2b1206af6aeaec51b5a1e77cf93ef266c970116ec84536fb865e2ffc5afe  include/uapi/linux/netfilter/ipset/ip_set_list.h
4465021663a5fa8aea1a5adfc6c3c68be2fbfc8e75ca3ab37e42d89f3c818703  include/uapi/linux/netfilter/xt_set.h
fef2d5ef45e7513bef08b03c7d7f5f8e805ea69b1d8729fc1fad5426ede98642  net/netfilter/ipset/ip_set_core.c
18e3cf404406f622b015c1b20f3001c83cf55e2e954b6fc815950e3735e15fa4  net/netfilter/ipset/ip_set_bitmap_gen.h
f79879d12d2ae9e00e151851258800d2998298b2d5ebac0940955fead1618c7c  net/netfilter/ipset/ip_set_hash_gen.h
068664e978f8d6196378a6fe5e7edfbea72c418fb9be14a1471c7dd03f9d682b  net/netfilter/ipset/ip_set_hash_ip.c
5d7deec753a1e7fa0a5ef18694d2673552c59eb999049beb4f4a0c51ebf2844e  net/netfilter/ipset/ip_set_hash_net.c
7c7f1c0eafd92dd972ed0a8ea94e8dd31324b4ba0c73ea2572f1aae8e1892eb6  net/netfilter/ipset/pfxlen.c
2845f65052c450f0f7d4065744bfb04db3d2eb738bafde096fc17517c8682cb6  net/netfilter/xt_set.c
```

Local copies of `ip_set_core.c`, `ip_set_hash_ip.c`, `ip_set_hash_net.c` and
`xt_set.c` intentionally differ; the other files match the hashes above.

## Known build/runtime dependency

The module uses only exported kernel APIs (`nfnetlink_subsys_register`,
`nfnetlink_subsys_unregister`, `nfnl_lock`, `nfnl_unlock`,
`nfnetlink_unicast`, `nf_register_sockopt`, `nf_unregister_sockopt`,
`register_pernet_subsys`, `unregister_pernet_subsys`, `xt_register_matches`,
`xt_unregister_matches`, `xt_register_targets`, `xt_unregister_targets`). No
unexported symbol shim is used. On the target GKI these live in built-in
`vmlinux` objects, but presence in the DDK `Module.symvers` is still a build
risk until the first CI build.
