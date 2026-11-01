/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include "yttrium_present.h"

#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

#include "gdikmt/gdikmt.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"

#include "yttrium_context.h"
#include "yttrium_gdi_public.h"
#include "yttrium_internal.h"
#include "yttrium_resource.h"
#include "yttrium_trace.h"
#include "yttrium_venus.h"

static struct pipe_context *
yttrium_present_driver_context(struct pipe_context *ctx)
{
   if (!ctx || !ctx->callback)
      return ctx;

   struct pipe_context *driver = threaded_context(ctx)->pipe;
   return driver && driver->flush == yttrium_flush ? driver : ctx;
}

struct yttrium_present_publication {
   struct yttrium_gdi_flush_issuance *issuance;
   struct gdikmt_context *producer;
   uint64_t value;
};

static HRESULT
yttrium_prepare_present_publication(void *data)
{
   struct yttrium_present_publication *publication = data;

   if (!publication)
      return E_FAIL;
   if (publication->issuance &&
       !yttrium_gdi_wait_flush_issuance(publication->issuance))
      return E_FAIL;
   if (publication->producer)
      return publication->producer->wait_present(publication->producer,
                                                 publication->value);
   return publication->issuance ? S_OK : E_FAIL;
}

void
yttrium_flush_frontbuffer(struct pipe_screen *pscreen,
                          struct pipe_context *ctx,
                          struct pipe_resource *resource,
                          unsigned level, unsigned layer,
                          void *context_private,
                          unsigned nboxes, struct pipe_box *boxes)
{
   struct yttrium_screen *screen = yttrium_screen(pscreen);
   struct yttrium_resource *res = yttrium_resource(resource);
   struct pipe_context *frontend_ctx = ctx;
   const struct gdikmt_present_info *present_info =
      gdikmt_present_info_from_context(context_private);
   struct gdikmt_present_info *present_status =
      (struct gdikmt_present_info *)present_info;
   void *dxgi_context_private =
      present_info ? present_info->dxgi_context : context_private;
   const D3DKMT_PRESENT *present =
      (const D3DKMT_PRESENT *)dxgi_context_private;

   struct pipe_context *driver_ctx = yttrium_present_driver_context(ctx);
   struct yttrium_context *yctx = yttrium_context(driver_ctx);

   if (!res || !res->hAllocation || !yctx || !yctx->kmt_ctx) {
      if (present_status)
         present_status->status = E_FAIL;
      YTTRIUM_WARN("yttrium: present failed, missing display allocation or context\n");
      return;
   }

   /* These runtime-shared window inputs are canonical GPU storage. The
    * compositor consumes them through pfnPresentCb; no scanout command may
    * reinterpret their opaque layout as linear pixels. */
   if (res->optimal_shared_image && res->display_target &&
       (!present_info || !present || level || layer ||
        !present->Flags.RedirectedFlip || present->Flags.Flip ||
        present->Flags.Blt || present->Flags.RedirectedBlt ||
        present_info->application_scanout || present_info->hDstAllocation)) {
      YTTRIUM_WARN("yttrium: ERROR optimal composition Present rejected owner=yttrium-present component=optimal-shared reason=only-redirected-flip-qualified flags=0x%x allocation=0x%lx action=fail-present\n",
                   present ? present->Flags.Value : 0,
                   (unsigned long)res->hAllocation);
      if (present_status)
         present_status->status = E_INVALIDARG;
      return;
   }

   const bool legacy_endpoint =
      yttrium_resource_is_legacy_optimal_present(res);
   /* Local legacy outputs publish after a canonical GPU copy/draw, including
    * the NULL-context DXGI Blt route. Primary and opaque composition contracts
    * retain their stricter flip-only qualification above/below. */
   if (res->optimal_present_image &&
       (level || layer ||
        (legacy_endpoint && present_info && present_info->hDstAllocation) ||
        (!legacy_endpoint &&
         (!present_info || !present ||
          !(present->Flags.Flip || present->Flags.RedirectedFlip) ||
          present->Flags.Blt || present->Flags.RedirectedBlt)))) {
      YTTRIUM_WARN("yttrium: ERROR optimal display Present rejected owner=yttrium-present component=linear-endpoint role=%s reason=%s flags=0x%x allocation=0x%lx action=fail-present\n",
                   legacy_endpoint ? "local-legacy" : "primary",
                   legacy_endpoint ? "legacy-destination-or-subresource-not-qualified" :
                      "only-flip-consumers-qualified",
                   present ? present->Flags.Value : 0,
                   (unsigned long)res->hAllocation);
      if (present_status)
         present_status->status = E_INVALIDARG;
      InterlockedExchange(&yctx->draw_failure, 1);
      gdikmt_device_report_reset(screen->device, PIPE_UNKNOWN_CONTEXT_RESET);
      return;
   }

   /* Publication is ordered with rendering on the same worker. Its copy must
    * precede the existing producer-ready signal, including the NULL-context
    * Present-blt route which has no normal Present callback below. */
   if (res->optimal_present_image &&
       !yttrium_gdi_publish_display_endpoint(frontend_ctx, resource,
                                              "optimal display Present")) {
      if (present_status)
         present_status->status = E_FAIL;
      return;
   }

   if (!context_private) {
      yttrium_gdi_flush_labeled(frontend_ctx, NULL, PIPE_FLUSH_ASYNC,
                                "display frontbuffer publication");
      return;
   }

   struct gdikmt_context *present_ctx = yctx->kmt_ctx;
   if (yttrium_resource_is_venus_backed_display(res)) {
      present_ctx = yttrium_venus_get_kmt_context(screen->venus);
      if (!present_ctx) {
         if (present_status)
            present_status->status = E_FAIL;
         YTTRIUM_WARN("yttrium: KMD present skipped, missing Venus render context hAllocation=0x%lx res_id=%u primary=%u\n",
                     (unsigned long)res->hAllocation,
                     res->venus_res_id,
                     res->primary_target);
         return;
      }
   }

   /* Direct application scanout updates must follow GPU completion, so hand
    * them to the ordered worker and let that worker issue the scanout escape.
    * Windowed and DWM presents retain the ordinary pfnPresentCb path. */
   const bool scheduled = present_ctx->scheduled_present_mode != 0;
   struct gdikmt_context *producer = present_ctx;
   const bool worker_publish =
      !scheduled && present_info &&
      present_info->version >= 3 && present_info->application_scanout;

   if (worker_publish) {
      const struct yttrium_gdi_present_publish_request publish = {
         .allocation = (uint32_t)res->hAllocation,
         .scanout_id = 0,
         .valid = true,
      };

      if (!yttrium_gdi_flush_async_present(
             frontend_ctx, "display Present worker publication", &publish, NULL)) {
         if (present_status)
            present_status->status = E_FAIL;
         return;
      }

      /*
       * The worker owns the only publication path.  Calling pfnPresentCb here
       * would let the miniport race an unordered scanout update against the
       * completion-ordered escape.
       */
      if (present_status)
         present_status->status = S_OK;
      return;
   }

   struct yttrium_gdi_flush_issuance *issuance = NULL;
   struct yttrium_present_publication publication = {0};

   if (scheduled) {
      HRESULT hr = producer->reserve_present(producer, &present_ctx, &publication.value);
      if (FAILED(hr)) {
         YTTRIUM_WARN("yttrium: ERROR scheduled Present owner=yttrium-present operation=reserve hr=%lx\n", (unsigned long)hr);
         if (present_status) present_status->status = hr;
         return;
      }
      publication.producer = producer;
      const struct yttrium_gdi_present_publish_request publish = {
         .allocation = (uint32_t)res->hAllocation,
         .valid = true,
         .signal_context = producer,
         .signal_value = publication.value,
      };
      if (!yttrium_gdi_flush_async_present(frontend_ctx, "scheduled Present signal",
             &publish, &issuance)) {
         if (present_status) present_status->status = E_FAIL;
         return;
      }
   } else {
      issuance = yttrium_gdi_flush_async_issuance(
         frontend_ctx, "display Present publication");
      if (!issuance) {
         if (present_status)
            present_status->status = E_FAIL;
         return;
      }
   }

   publication.issuance = issuance;

   struct gdikmt_present_info device_present_info;
   memset(&device_present_info, 0, sizeof(device_present_info));
   device_present_info.magic = GDIKMT_PRESENT_INFO_MAGIC;
   device_present_info.version = 2;
   device_present_info.dxgi_context = dxgi_context_private;
   device_present_info.hDstAllocation =
      present_info ? present_info->hDstAllocation : 0;
   device_present_info.status = S_OK;
   device_present_info.prepare_present = yttrium_prepare_present_publication;
   device_present_info.prepare_present_data = &publication;

   NTSTATUS status = screen->device->present(present_ctx, res->hAllocation,
                                             &device_present_info, boxes);
   yttrium_gdi_flush_issuance_release(issuance);
   if (present_status)
      present_status->status = status;
   if (!NT_SUCCESS(status)) {
      YTTRIUM_WARN("yttrium: present callback path failed status=0x%lx hAllocation=0x%lx res_id=%u dxgi_owner=0x%lx hWindow=%p dxgi_source=0x%lx dxgi_destination=0x%lx flags=0x%x flip_interval=%u present_count=%u\n",
                   status,
                   (unsigned long)res->hAllocation,
                   res->venus_res_id,
                   present ? (unsigned long)present->hContext : 0,
                   present ? present->hWindow : NULL,
                   present ? (unsigned long)present->hSource : 0,
                   present ? (unsigned long)present->hDestination : 0,
                   present ? present->Flags.Value : 0,
                   present ? present->FlipInterval : 0,
                   present ? present->PresentCount : 0);
   }
}

bool
yttrium_ensure_display_mapped(struct pipe_context *ctx,
                              struct yttrium_resource *res)
{
   if (res->map)
      return true;

   return yttrium_map_display_allocation(ctx->screen, res);
}
