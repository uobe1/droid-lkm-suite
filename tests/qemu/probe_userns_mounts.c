// SPDX-License-Identifier: GPL-3.0-or-later
/* Step-by-step errno probe mirroring mount_setattr_test's fixture. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

static void show(const char *path)
{
	struct stat st;

	if (!stat(path, &st))
		printf("PROBE stat %s mode=%o uid=%d gid=%d\n", path,
		       st.st_mode, st.st_uid, st.st_gid);
}
#include <unistd.h>

static void dump_status(const char *tag)
{
	char buf[4096];
	int fd = open("/proc/self/status", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	printf("PROBE %s uid=%d euid=%d\n", tag, getuid(), geteuid());
	for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n"))
		if (!strncmp(l, "CapEff:", 7) || !strncmp(l, "Uid:", 4) ||
		    !strncmp(l, "NoNewPrivs:", 11))
			printf("PROBE %s %s\n", tag, l);
}

static int step(const char *name, int rc)
{
	if (rc < 0) {
		printf("PROBE %s FAIL errno=%d (%s)\n", name, errno,
		       strerror(errno));
		return -1;
	}
	printf("PROBE %s ok\n", name);
	return 0;
}

static int write_file(const char *path, const char *buf, size_t len)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, buf, len);
	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

int main(void)
{
	char map[64];

	dump_status("before");
	{
		char b[64];
		int f = open("/proc/self/attr/current", O_RDONLY);
		int n = (f >= 0) ? read(f, b, 63) : -1;

		if (n > 0) {
			b[n] = 0;
			printf("PROBE SID=%s", b);
		}
		if (f >= 0)
			close(f);
	}
	uid_t uid = getuid();
	gid_t gid = getgid();
	if (step("unshare_user", unshare(CLONE_NEWUSER)))
		return 1;
	snprintf(map, sizeof(map), "0 %d 1", uid);
	if (step("setgroups", write_file("/proc/self/setgroups", "deny", 4)) &&
	    errno != ENOENT)
		return 1;
	if (step("uid_map", write_file("/proc/self/uid_map", map, strlen(map)))) {
		int alt = write_file("/proc/self/uid_map", "1 0 1", 5);
		printf("PROBE uid_map_alt rc=%d errno=%d (%s)\n", alt, errno,
		       strerror(errno));
		return 1;
	}
	snprintf(map, sizeof(map), "0 %d 1", gid);
	if (step("gid_map", write_file("/proc/self/gid_map", map, strlen(map))))
		return 1;
	if (step("setgid", setgid(0)))
		return 1;
	if (step("setuid", setuid(0)))
		return 1;
	if (step("unshare_mnt", unshare(CLONE_NEWNS)))
		return 1;
	if (step("make_private", mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, 0)))
		return 1;
	(void)umount2("/mnt", MNT_DETACH);
	(void)umount2("/tmp", MNT_DETACH);
	if (step("mnt_tmpfs", mount("testing", "/tmp", "tmpfs",
				    MS_NOATIME | MS_NODEV,
				    "size=100000,mode=700")))
		return 1;
	if (step("mkdir_tmpB", mkdir("/tmp/B", 0777)))
		return 1;
	if (step("mnt_tmpB", mount("testing", "/tmp/B", "tmpfs",
				   MS_NOATIME | MS_NODEV,
				   "size=100000,mode=700")))
		return 1;
	if (step("mkdir_tmpBB", mkdir("/tmp/B/BB", 0777)))
		return 1;
	if (step("mnt_tmpBB", mount("testing", "/tmp/B/BB", "tmpfs",
				    MS_NOATIME | MS_NODEV,
				    "size=100000,mode=700")))
		return 1;
	if (step("mnt_mnt", mount("testing", "/mnt", "tmpfs",
				  MS_NOATIME | MS_NODEV,
				  "size=100000,mode=700")))
		return 1;
	if (step("mkdir_mntA", mkdir("/mnt/A", 0777)))
		return 1;
	if (step("mnt_mntA", mount("testing", "/mnt/A", "tmpfs",
				   MS_NOATIME | MS_NODEV,
				   "size=100000,mode=700")))
		return 1;
	if (step("mkdir_mntAA", mkdir("/mnt/A/AA", 0777)))
		return 1;
	if (step("bind_AA", mount("/tmp", "/mnt/A/AA", NULL,
				  MS_BIND | MS_REC, NULL)))
		return 1;
	if (step("mkdir_mntB", mkdir("/mnt/B", 0777)))
		return 1;
	if (step("mnt_mntB_ramfs", mount("testing", "/mnt/B", "ramfs",
					 MS_NOATIME | MS_NODEV | MS_NOSUID, 0)))
		return 1;
	show("/mnt");
	show("/mnt/B");
	{
		int fd = open("/mnt/B/f", O_CREAT | O_RDWR, 0600);
		printf("PROBE create_file rc=%d errno=%d (%s)\n", fd, errno,
		       strerror(errno));
		if (fd >= 0)
			close(fd);
	}
	printf("PROBE chmod_ramfs rc=%d errno=%d (%s)\n",
	       chmod("/mnt/B", 0777), errno, strerror(errno));
	show("/mnt/B");
	if (step("mkdir_mntBB", mkdir("/mnt/B/BB", 0777)))
		return 1;
	printf("PROBE_ALL_OK\n");
	return 0;
}
