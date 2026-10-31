// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/stdarg.h>
#include "dnsk_internal.h"

void dnsk_log(int level, const char *fmt, ...)
{
	char buf[256];
	va_list args;

	if (!fmt)
		return;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	switch (level) {
	case 0:
		dnsk_err("%s\n", buf);
		break;
	case 1:
		dnsk_warn("%s\n", buf);
		break;
	default:
		dnsk_info("%s\n", buf);
		break;
	}
}
