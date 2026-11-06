# Vendored kernel selftests

Source: AOSP `kernel/common`, branch `android16-6.12`, commit
`105b5745f1d771060aa9fb7f75821f69bb2298fd`.

- `mount_setattr/mount_setattr_test.c`
- `kselftest_harness.h`
- `kselftest.h`

These are unmodified upstream files (GPL-2.0).  They are built statically
and run inside the beryl Image user-namespace gate; the test itself enters
a user namespace, so it exercises the emulated `create_user_ns`,
`mount_setattr` idmap handling and the mount API.
