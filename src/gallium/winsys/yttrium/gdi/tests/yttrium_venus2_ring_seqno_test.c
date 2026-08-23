/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdint.h>
#include <stdio.h>

#include "yttrium_venus2_ring_seqno.h"

static unsigned failures;

static void
expect(bool condition, const char *description)
{
   if (condition)
      return;

   fprintf(stderr, "FAIL: %s\n", description);
   failures++;
}

int
main(void)
{
   const struct yttrium_venus_ring_dependency none =
      yttrium_venus_ring_dependency_none();
   const struct yttrium_venus_ring_dependency wrapped_zero =
      yttrium_venus_ring_dependency_create(0);
   const struct yttrium_venus_ring_dependency before_wrap =
      yttrium_venus_ring_dependency_create(UINT32_MAX - 15u);
   const struct yttrium_venus_ring_dependency after_wrap =
      yttrium_venus_ring_dependency_create(16u);

   expect(!yttrium_venus_ring_dependency_valid(&none),
          "an absent dependency is not waitable");
   expect(!yttrium_venus_ring_dependency_reached(0, &none),
          "an absent dependency is not complete at cursor zero");

   expect(yttrium_venus_ring_dependency_valid(&wrapped_zero),
          "wrapped cursor zero remains eligible for an exact wait");
   expect(!yttrium_venus_ring_dependency_reached(UINT32_MAX,
                                                  &wrapped_zero),
          "wrapped cursor zero is pending immediately before wrap");
   expect(yttrium_venus_ring_dependency_reached(0, &wrapped_zero),
          "poll completes wrapped cursor zero exactly at wrap");
   expect(yttrium_venus_ring_dependency_reached(8, &wrapped_zero),
          "poll keeps wrapped cursor zero complete after wrap");

   expect(!yttrium_venus_ring_dependency_reached(UINT32_MAX - 16u,
                                                  &before_wrap),
          "pre-wrap dependency is pending one byte before its cursor");
   expect(yttrium_venus_ring_dependency_reached(UINT32_MAX - 15u,
                                                 &before_wrap),
          "pre-wrap dependency completes at its exact cursor");
   expect(yttrium_venus_ring_dependency_reached(0, &before_wrap),
          "pre-wrap dependency remains complete across cursor wrap");

   expect(!yttrium_venus_ring_dependency_reached(UINT32_MAX - 15u,
                                                  &after_wrap),
          "post-wrap dependency is not complete before cursor wrap");
   expect(!yttrium_venus_ring_dependency_reached(15u, &after_wrap),
          "post-wrap dependency remains pending before its exact cursor");
   expect(yttrium_venus_ring_dependency_reached(16u, &after_wrap),
          "post-wrap dependency completes at its exact cursor");

   if (failures) {
      fprintf(stderr, "%u ring sequence test(s) failed\n", failures);
      return 1;
   }

   puts("yttrium Venus2 ring sequence wrap tests passed");
   return 0;
}
