// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Device-side preflight.  This is intentionally conservative: it checks the
 * facts dnsk_core depends on and refuses to report success when a required
 * symbol or environment fact is missing.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct sym_req {
	const char *name;
	int found;
};

static struct sym_req required[] = {
	{ "sys_call_table", 0 },
	{ "init_mm", 0 },
	{ "panic_notifier_list", 0 },
	{ "stop_machine", 0 },
	{ "register_kprobe", 0 },
	{ "kallsyms_lookup_name", 0 },
	{ "kallsyms_on_each_symbol", 0 },
	{ "dcache_clean_inval_poc", 0 },
	{ "__flush_dcache_area", 0 },
};

static int find_symbol(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(required); i++)
		if (!strcmp(required[i].name, name))
			return (int)i;
	return -1;
}

static int scan_kallsyms(void)
{
	FILE *fp;
	char line[512];

	fp = fopen("/proc/kallsyms", "r");
	if (!fp)
		return -1;
	while (fgets(line, sizeof(line), fp)) {
		char type;
		char name[256];
		int idx;

		if (sscanf(line, "%*s %c %255s", &type, name) != 2)
			continue;
		idx = find_symbol(name);
		if (idx >= 0)
			required[idx].found = 1;
	}
	fclose(fp);
	return 0;
}

static int parse_kmi(const char *release, char *out, size_t out_len)
{
	const char *a = strstr(release, "android");
	char *end;
	int android;
	int major, minor;

	if (!a)
		return -1;
	android = (int)strtol(a + strlen("android"), &end, 10);
	if (end == a + strlen("android"))
		return -1;
	if (sscanf(release, "%d.%d", &major, &minor) != 2)
		return -1;
	if (snprintf(out, out_len, "android%d-%d.%d", android, major, minor) >=
	    (int)out_len)
		return -1;
	return 0;
}

static int supported_kmi(const char *path, const char *kmi)
{
	FILE *fp;
	char line[128];
	int found = 0;

	if (!path || !path[0])
		return 0;
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strcmp(line, kmi)) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	return found;
}

static char *find_ksud(void)
{
	static char path[PATH_MAX];
	const char *env = getenv("DNSK_KSUD");
	const char *candidates[] = {
		"/data/adb/ksud",
		"/data/adb/ksu/bin/ksud",
	};

	if (env && access(env, X_OK) == 0) {
		snprintf(path, sizeof(path), "%s", env);
		return path;
	}
	for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (access(candidates[i], X_OK) == 0) {
			snprintf(path, sizeof(path), "%s", candidates[i]);
			return path;
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct utsname uts;
	char kmi[64] = { 0 };
	char derived[64] = { 0 };
	const char *supported_path = NULL;
	const char *ksud;
	unsigned long page_size;
	int btf;
	int supported;
	int dcache;
	int resolver;
	int ok;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--kmi") && i + 1 < argc)
			snprintf(kmi, sizeof(kmi), "%s", argv[++i]);
		else if (!strcmp(argv[i], "--supported") && i + 1 < argc)
			supported_path = argv[++i];
	}

	if (uname(&uts) != 0) {
		perror("uname");
		return 1;
	}
	if (!kmi[0] && parse_kmi(uts.release, derived, sizeof(derived)) == 0)
		snprintf(kmi, sizeof(kmi), "%s", derived);

	page_size = (unsigned long)sysconf(_SC_PAGESIZE);
	btf = access("/sys/kernel/btf/vmlinux", R_OK) == 0;
	ksud = find_ksud();
	supported = supported_kmi(supported_path, kmi);
	scan_kallsyms();

	dcache = required[7].found || required[8].found;
	resolver = required[5].found || required[6].found;
	ok = page_size == 4096 && btf && ksud && supported && kmi[0] &&
	     required[0].found && required[1].found && required[2].found &&
	     required[3].found && required[4].found && dcache && resolver;

	printf("{\"ok\":%s,\"release\":\"%s\",\"kmi\":\"%s\","
	       "\"supported\":%s,\"page_size\":%lu,\"btf\":%s,"
	       "\"ksud\":\"%s\",\"symbols\":{",
	       ok ? "true" : "false", uts.release, kmi,
	       supported ? "true" : "false", page_size,
	       btf ? "true" : "false", ksud ? ksud : "");
	for (size_t i = 0; i < ARRAY_SIZE(required); i++) {
		printf("%s\"%s\":%s", i ? "," : "", required[i].name,
		       required[i].found ? "true" : "false");
	}
	printf("},\"checks\":{\"page_size_4k\":%s,\"btf\":%s,"
	       "\"kmi_supported\":%s,\"ksud\":%s,\"dcache\":%s,"
	       "\"kallsyms_resolver\":%s}}\n",
	       page_size == 4096 ? "true" : "false", btf ? "true" : "false",
	       supported ? "true" : "false", ksud ? "true" : "false",
	       dcache ? "true" : "false", resolver ? "true" : "false");
	return ok ? 0 : 1;
}
