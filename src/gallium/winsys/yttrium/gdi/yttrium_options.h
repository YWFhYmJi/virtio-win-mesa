/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef YTTRIUM_OPTIONS_H
#define YTTRIUM_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool
yttrium_gdi_ascii_iequals(const char *a, const char *b);

const char *
yttrium_gdi_debug_get_option(const char *name, const char *dfault);

bool
yttrium_gdi_debug_get_bool_option(const char *name, bool dfault);

bool
yttrium_gdi_static_ubo_sampled_cache_enabled(void);

int64_t
yttrium_gdi_debug_get_num_option(const char *name, int64_t dfault);

void
yttrium_gdi_debug_get_config_status(bool *loaded,
                                    bool *found,
                                    const char **path,
                                    unsigned *entry_count);

#ifdef __cplusplus
}
#endif

#endif /* YTTRIUM_OPTIONS_H */
