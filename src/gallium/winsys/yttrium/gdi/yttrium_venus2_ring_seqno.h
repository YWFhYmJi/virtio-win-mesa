/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef YTTRIUM_VENUS2_RING_SEQNO_H
#define YTTRIUM_VENUS2_RING_SEQNO_H

#include <stdbool.h>
#include <stdint.h>

/* A ring cursor value is not its validity.  In particular, zero is a valid
 * command-end cursor after the cumulative 32-bit byte cursor wraps. */
struct yttrium_venus_ring_dependency {
   uint32_t seqno;
   bool valid;
};

static inline struct yttrium_venus_ring_dependency
yttrium_venus_ring_dependency_none(void)
{
   struct yttrium_venus_ring_dependency dependency;
   dependency.seqno = 0;
   dependency.valid = false;
   return dependency;
}

static inline struct yttrium_venus_ring_dependency
yttrium_venus_ring_dependency_create(uint32_t seqno)
{
   struct yttrium_venus_ring_dependency dependency;
   dependency.seqno = seqno;
   dependency.valid = true;
   return dependency;
}

static inline bool
yttrium_venus_ring_dependency_valid(
   const struct yttrium_venus_ring_dependency *dependency)
{
   return dependency && dependency->valid;
}

static inline bool
yttrium_venus_ring_cursor_reached(uint32_t head, uint32_t seqno)
{
   /* Valid outstanding work is bounded by the ring size and therefore stays
    * far below the 2^31 ambiguity boundary. */
   return head == seqno || (uint32_t)(head - seqno) < 0x80000000u;
}

static inline bool
yttrium_venus_ring_dependency_reached(
   uint32_t head,
   const struct yttrium_venus_ring_dependency *dependency)
{
   return yttrium_venus_ring_dependency_valid(dependency) &&
          yttrium_venus_ring_cursor_reached(head, dependency->seqno);
}

#endif /* YTTRIUM_VENUS2_RING_SEQNO_H */
