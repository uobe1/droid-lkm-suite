/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DNSK_KSUD_HINT_H
#define DNSK_KSUD_HINT_H

#include "core.h"

enum kallrecon_fail {
	KALLRECON_OK = 0,
};

struct kallrecon_hint {
	unsigned int timeout_ms;
};

int kallrecon_supply(const struct kallrecon_hint *hint);
enum kallrecon_fail kallrecon_fail_reason(void);

#endif /* DNSK_KSUD_HINT_H */
