// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Device-side preflight.  This is intentionally conservative: it checks the
 * facts dnsk_core depends on and refuses to report success when a required
 * symbol or environment fact is missing.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/*
 * Android Bionic requires a 64-byte-aligned PT_TLS for arm64.  A static
 * clang binary may otherwise be rejected by the loader ("TLS segment is
 * underaligned") even when the program itself has no TLS.  This aligned
 * thread-local object forces the linker's PT_TLS alignment to 64.
 */
#if defined(__ANDROID__) && defined(__aarch64__)
__attribute__((visibility("default"), aligned(64)))
_Thread_local int dnsk_android_tls_alignment_fix = 0;
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct btf_layout {
	uint32_t xt_match_size;
	uint32_t xt_target_size;
	int found_match;
	int found_target;
};

static uint32_t btf_rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t btf_rd16(const unsigned char *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static const char *btf_str(const unsigned char *strs, size_t str_len,
			   uint32_t off)
{
	const char *s;

	if (off >= str_len)
		return NULL;
	s = (const char *)strs + off;
	if (!memchr(s, 0, str_len - off))
		return NULL;
	return s;
}

static int btf_find_struct(const unsigned char *d, size_t len,
			   const char *want, uint32_t *out_size)
{
	uint32_t hdr_len, type_off, type_len, str_off, str_len;
	const unsigned char *types;
	const unsigned char *strs;
	size_t pos = 0;

	if (len < 24 || btf_rd16(d) != 0xeb9f || d[2] != 1)
		return -1;
	hdr_len = btf_rd32(d + 4);
	type_off = btf_rd32(d + 8);
	type_len = btf_rd32(d + 12);
	str_off = btf_rd32(d + 16);
	str_len = btf_rd32(d + 20);
	if (hdr_len > len || type_off > len - hdr_len ||
	    type_len > len - hdr_len - type_off ||
	    str_off > len - hdr_len || str_len > len - hdr_len - str_off)
		return -1;
	types = d + hdr_len + type_off;
	strs = d + hdr_len + str_off;

	while (pos + 12 <= type_len) {
		uint32_t name_off = btf_rd32(types + pos);
		uint32_t info = btf_rd32(types + pos + 4);
		uint32_t size_type = btf_rd32(types + pos + 8);
		unsigned int kind = (info >> 24) & 0x1f;
		unsigned int vlen = info & 0xffff;
		const char *name = btf_str(strs, str_len, name_off);

		pos += 12;
		if (name && kind == 4 && !strcmp(name, want)) {
			*out_size = size_type;
			return 0;
		}
		switch (kind) {
		case 1: /* INT */
		case 14: /* VAR */
		case 17: /* DECL_TAG */
			pos += 4;
			break;
		case 3: /* ARRAY */
			pos += 12;
			break;
		case 4: /* STRUCT */
		case 5: /* UNION */
		case 15: /* DATASEC */
			pos += (size_t)vlen * 12;
			break;
		case 6: /* ENUM */
		case 13: /* FUNC_PROTO */
			pos += (size_t)vlen * 8;
			break;
		case 19: /* ENUM64 */
			pos += (size_t)vlen * 12;
			break;
		default:
			break;
		}
		if (pos > type_len)
			return -1;
	}
	return -1;
}

static int btf_read_layout(const char *path, struct btf_layout *layout)
{
	FILE *fp;
	long size;
	unsigned char *data;
	size_t got;

	memset(layout, 0, sizeof(*layout));
	fp = fopen(path, "rb");
	if (!fp)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	size = ftell(fp);
	if (size <= 0 || size > 64 * 1024 * 1024) {
		fclose(fp);
		return -1;
	}
	rewind(fp);
	data = malloc((size_t)size);
	if (!data) {
		fclose(fp);
		return -1;
	}
	got = fread(data, 1, (size_t)size, fp);
	fclose(fp);
	if (got != (size_t)size) {
		free(data);
		return -1;
	}
	layout->found_match =
		btf_find_struct(data, (size_t)size, "xt_match",
				&layout->xt_match_size) == 0;
	layout->found_target =
		btf_find_struct(data, (size_t)size, "xt_target",
				&layout->xt_target_size) == 0;
	free(data);
	return 0;
}

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
	const char *btf_path = "/sys/kernel/btf/vmlinux";
	long xt_expected = -1;
	struct btf_layout layout;
	int btf;
	int layout_ok;
	int supported;
	int dcache;
	int resolver;
	int ok;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--kmi") && i + 1 < argc)
			snprintf(kmi, sizeof(kmi), "%s", argv[++i]);
		else if (!strcmp(argv[i], "--supported") && i + 1 < argc)
			supported_path = argv[++i];
		else if (!strcmp(argv[i], "--btf") && i + 1 < argc)
			btf_path = argv[++i];
		else if (!strcmp(argv[i], "--xt-layout") && i + 1 < argc)
			xt_expected = strtol(argv[++i], NULL, 10);
	}

	if (uname(&uts) != 0) {
		perror("uname");
		return 1;
	}
	if (!kmi[0] && parse_kmi(uts.release, derived, sizeof(derived)) == 0)
		snprintf(kmi, sizeof(kmi), "%s", derived);

	page_size = (unsigned long)sysconf(_SC_PAGESIZE);
	btf = access(btf_path, R_OK) == 0;
	memset(&layout, 0, sizeof(layout));
	if (btf)
		btf_read_layout(btf_path, &layout);
	layout_ok = xt_expected < 0 ||
		    (layout.found_match && layout.found_target &&
		     layout.xt_match_size == (uint32_t)xt_expected &&
		     layout.xt_target_size == (uint32_t)xt_expected);
	ksud = find_ksud();
	supported = supported_kmi(supported_path, kmi);
	scan_kallsyms();

	dcache = required[7].found || required[8].found;
	resolver = required[5].found || required[6].found;
	ok = page_size == 4096 && btf && layout_ok && ksud && supported &&
	     kmi[0] && required[0].found && required[1].found &&
	     required[2].found && required[3].found && required[4].found &&
	     dcache && resolver;

	printf("{\"ok\":%s,\"release\":\"%s\",\"kmi\":\"%s\","
	       "\"supported\":%s,\"page_size\":%lu,\"btf\":%s,"
	       "\"ksud\":\"%s\",\"btf_layout\":{"
	       "\"xt_match_size\":%u,\"xt_target_size\":%u,"
	       "\"xt_ok\":%s},\"symbols\":{",
	       ok ? "true" : "false", uts.release, kmi,
	       supported ? "true" : "false", page_size,
	       btf ? "true" : "false", ksud ? ksud : "",
	       layout.xt_match_size, layout.xt_target_size,
	       layout_ok ? "true" : "false");
	for (size_t i = 0; i < ARRAY_SIZE(required); i++) {
		printf("%s\"%s\":%s", i ? "," : "", required[i].name,
		       required[i].found ? "true" : "false");
	}
	printf("},\"checks\":{\"page_size_4k\":%s,\"btf\":%s,"
	       "\"kmi_supported\":%s,\"ksud\":%s,\"dcache\":%s,"
	       "\"kallsyms_resolver\":%s,\"xt_layout\":%s}}\n",
	       page_size == 4096 ? "true" : "false", btf ? "true" : "false",
	       supported ? "true" : "false", ksud ? "true" : "false",
	       dcache ? "true" : "false", resolver ? "true" : "false",
	       layout_ok ? "true" : "false");
	return ok ? 0 : 1;
}
