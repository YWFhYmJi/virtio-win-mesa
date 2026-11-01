/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef YTTRIUM_VENUS2_SHARED_IMAGE_H
#define YTTRIUM_VENUS2_SHARED_IMAGE_H

#include <stdbool.h>

/* Tracks the canonical render image and its read-only display endpoint in the
 * ordered command stream, including accepted pending batches. Neither flag
 * establishes GPU completion or permission to read on the CPU. The current
 * recording snapshots this state and restores it if abandoned. */
struct yttrium_venus_shared_image_state {
   bool private_current;
   bool public_current;
};

static inline bool
yttrium_venus_shared_image_publish_needed(
   const struct yttrium_venus_shared_image_state *state)
{
   return state->private_current && !state->public_current;
}

static inline void
yttrium_venus_shared_image_written(
   struct yttrium_venus_shared_image_state *state)
{
   state->private_current = true;
   state->public_current = false;
}

static inline void
yttrium_venus_shared_image_published(
   struct yttrium_venus_shared_image_state *state)
{
   state->public_current = true;
}

#endif
