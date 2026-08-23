/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Draw.h --
 *    Functions that render 3D primitives.
 */


#include "Draw.h"
#include "State.h"
#include "Shader.h"

#include "Debug.h"

#include "util/u_draw.h"
#include "util/u_memory.h"

#include "gallium/winsys/yttrium/gdi/yttrium_gdi_public.h"

static bool
OrderedContextWorkerEnabled()
{
   static int enabled = -1;

   if (enabled < 0) {
      enabled = yttrium_gdi_debug_get_bool_option(
         "D3D10UMD_YTTRIUM_ORDERED_CONTEXT_WORKER", true) ? 1 : 0;
   }
   return enabled != 0;
}

static bool
ReadBufferRange(struct pipe_context *pipe, Resource *resource,
                unsigned offset, unsigned size, void *data)
{
   if (!pipe || !resource || !resource->resource ||
       resource->resource->target != PIPE_BUFFER || !data ||
       offset > resource->resource->width0 ||
       size > resource->resource->width0 - offset)
      return false;

   struct pipe_box box = {};
   box.x = offset;
   box.width = size;
   box.height = 1;
   box.depth = 1;

   struct pipe_transfer *transfer = NULL;
   void *map = pipe->buffer_map(pipe, resource->resource, 0, PIPE_MAP_READ,
                                &box, &transfer);
   if (map) {
      memcpy(data, map, size);
      pipe->buffer_unmap(pipe, transfer);
      return true;
   }

   if (resource->buffer_shadow &&
       offset <= resource->buffer_shadow_size &&
       size <= resource->buffer_shadow_size - offset) {
      memcpy(data, (const uint8_t *)resource->buffer_shadow + offset, size);
      return true;
   }

   return false;
}

static unsigned
ClampedUAdd(unsigned a,
            unsigned b)
{
   unsigned c = a + b;
   if (c < a) {
      return 0xffffffff;
   }
   return c;
}


/* stride is required in order to set the element data */
static void
update_velems(Device *pDevice)
{
   if (!pDevice->velems_changed)
      return;

   if(pDevice->element_layout) {
      struct cso_velems_state *state = &pDevice->element_layout->state;
      for (unsigned i = 0; i < state->count; i++)
         state->velems[i].src_stride = pDevice->vertex_strides[state->velems[i].vertex_buffer_index];
      cso_set_vertex_elements(pDevice->cso, state);
   }

   pDevice->velems_changed = false;
}

/*
 * We have to resolve the stream output state for empty geometry shaders.
 * In particular we've remapped the output indices when translating the
 * shaders so now the register_index variables in the stream output
 * state are incorrect and we need to remap them back to the correct
 * state.
 */
static void
ResolveState(Device *pDevice)
{
   RefreshBoundShaderResourceViews(pDevice);

   if (pDevice->bound_empty_gs && pDevice->bound_vs &&
       pDevice->bound_vs->state.tokens) {
      Shader *gs = pDevice->bound_empty_gs;
      Shader *vs = pDevice->bound_vs;
      bool remapped = false;
      struct pipe_context *pipe = pDevice->pipe;
      if (!gs->output_resolved) {
         for (unsigned i = 0; i < gs->state.stream_output.num_outputs; ++i) {
            unsigned mapping =
               ShaderFindOutputMapping(vs, gs->state.stream_output.output[i].register_index);
            if (mapping != gs->state.stream_output.output[i].register_index) {
               gs->state.stream_output.output[i].register_index = mapping;
               remapped = true;
            }
         }
         if (remapped) {
            pipe->delete_gs_state(pipe, gs->handle);
            gs->handle = pipe->create_gs_state(pipe, &gs->state);
         }
         gs->output_resolved = true;
      }
      pipe->bind_gs_state(pipe, gs->handle);
   }
   update_velems(pDevice);

   if (pDevice->vbuffers_changed) {
      unsigned count = PIPE_MAX_ATTRIBS;

      if (OrderedContextWorkerEnabled()) {
         count = 0;
         for (unsigned i = PIPE_MAX_ATTRIBS; i > 0; i--) {
            const struct pipe_vertex_buffer *vb =
               &pDevice->vertex_buffers[i - 1];
            if (vb->is_user_buffer ? vb->buffer.user != NULL :
                                     vb->buffer.resource != NULL) {
               count = i;
               break;
            }
         }
      }

      cso_set_vertex_buffers(pDevice->cso, count,
                             pDevice->vertex_buffers);
      pDevice->vbuffers_changed = false;
   }
}


static struct pipe_resource *
create_null_index_buffer(struct pipe_context *ctx, uint num_indices,
                         unsigned *restart_index, unsigned *index_size,
                         unsigned *ib_offset)
{
   unsigned buf_size = num_indices * sizeof(unsigned);
   unsigned *buf = (unsigned*)MALLOC(buf_size);
   struct pipe_resource *ibuf;

   memset(buf, 0, buf_size);

   ibuf = pipe_buffer_create_with_data(ctx,
                                       PIPE_BIND_INDEX_BUFFER,
                                       PIPE_USAGE_IMMUTABLE,
                                       buf_size, buf);
   *index_size = 4;
   *restart_index = 0xffffffff;
   *ib_offset = 0;

   FREE(buf);

   return ibuf;
}

/*
 * ----------------------------------------------------------------------
 *
 * Draw --
 *
 *    The Draw function draws nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Draw(D3D10DDI_HDEVICE hDevice,   // IN
     UINT VertexCount,           // IN
     UINT StartVertexLocation)   // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   ResolveState(pDevice);
   if (RunVertexShaderEmulation(pDevice, VertexCount))
      return;
   if (RunPixelShaderEmulation(pDevice))
      return;

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   util_draw_arrays(pDevice->pipe,
                    pDevice->primitive,
                    StartVertexLocation,
                    VertexCount);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexed --
 *
 *    The DrawIndexed function draws indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexed(D3D10DDI_HDEVICE hDevice,  // IN
            UINT IndexCount,           // IN
            UINT StartIndexLocation,   // IN
            INT BaseVertexLocation)    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCount,
                                  &restart_index, &index_size, &ib_offset);
   }

   ResolveState(pDevice);
   if (RunPixelShaderEmulation(pDevice)) {
      if (null_ib)
         pipe_resource_reference(&null_ib, NULL);
      return;
   }

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCount;
   info.index.resource = null_ib ? null_ib : pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.primitive_restart = true;
   info.restart_index = restart_index;

   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, NULL, &draw, 1);

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawInstanced --
 *
 *    The DrawInstanced function draws particular instances
 *    of nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawInstanced(D3D10DDI_HDEVICE hDevice,      // IN
              UINT VertexCountPerInstance,   // IN
              UINT InstanceCount,            // IN
              UINT StartVertexLocation,      // IN
              UINT StartInstanceLocation)    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   if (!InstanceCount) {
      return;
   }

   ResolveState(pDevice);
   if (RunPixelShaderEmulation(pDevice))
      return;

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   util_draw_arrays_instanced(pDevice->pipe,
                              pDevice->primitive,
                              StartVertexLocation,
                              VertexCountPerInstance,
                              StartInstanceLocation,
                              InstanceCount);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexedInstanced --
 *
 *    The DrawIndexedInstanced function draws particular
 *    instances of indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexedInstanced(D3D10DDI_HDEVICE hDevice,   // IN
                     UINT IndexCountPerInstance, // IN
                     UINT InstanceCount,         // IN
                     UINT StartIndexLocation,    // IN
                     INT BaseVertexLocation,     // IN
                     UINT StartInstanceLocation) // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   if (!InstanceCount) {
      return;
   }

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCountPerInstance,
                                  &restart_index, &index_size, &ib_offset);
   }

   ResolveState(pDevice);
   if (RunPixelShaderEmulation(pDevice)) {
      if (null_ib)
         pipe_resource_reference(&null_ib, NULL);
      return;
   }

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCountPerInstance;
   info.index.resource = null_ib ? null_ib : pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.start_instance = StartInstanceLocation;
   info.instance_count = InstanceCount;
   info.primitive_restart = true;
   info.restart_index = restart_index;

   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, NULL, &draw, 1);

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
}

void APIENTRY
DrawIndexedInstancedIndirect(D3D10DDI_HDEVICE hDevice,
                             D3D10DDI_HRESOURCE hBufferForArgs,
                             UINT AlignedByteOffsetForArgs)
{
   LOG_ENTRYPOINT();

   struct DrawIndexedInstancedIndirectArgs {
      UINT IndexCountPerInstance;
      UINT InstanceCount;
      UINT StartIndexLocation;
      INT BaseVertexLocation;
      UINT StartInstanceLocation;
   } args = {};

   Device *pDevice = CastDevice(hDevice);
   Resource *pArgs = CastResource(hBufferForArgs);
   if (!ReadBufferRange(pDevice->pipe, pArgs, AlignedByteOffsetForArgs,
                        sizeof(args), &args)) {
      LOG_UNSUPPORTED("DrawIndexedInstancedIndirect failed to read args");
      return;
   }

   DrawIndexedInstanced(hDevice, args.IndexCountPerInstance,
                        args.InstanceCount, args.StartIndexLocation,
                        args.BaseVertexLocation, args.StartInstanceLocation);
}

void APIENTRY
DrawInstancedIndirect(D3D10DDI_HDEVICE hDevice,
                      D3D10DDI_HRESOURCE hBufferForArgs,
                      UINT AlignedByteOffsetForArgs)
{
   LOG_ENTRYPOINT();

   struct DrawInstancedIndirectArgs {
      UINT VertexCountPerInstance;
      UINT InstanceCount;
      UINT StartVertexLocation;
      UINT StartInstanceLocation;
   } args = {};

   Device *pDevice = CastDevice(hDevice);
   Resource *pArgs = CastResource(hBufferForArgs);
   if (!ReadBufferRange(pDevice->pipe, pArgs, AlignedByteOffsetForArgs,
                        sizeof(args), &args)) {
      LOG_UNSUPPORTED("DrawInstancedIndirect failed to read args");
      return;
   }

   DrawInstanced(hDevice, args.VertexCountPerInstance, args.InstanceCount,
                 args.StartVertexLocation, args.StartInstanceLocation);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawAuto --
 *
 *    The DrawAuto function works similarly to the Draw function,
 *    except DrawAuto is used for the special case where vertex
 *    data is written through the stream-output unit and then
 *    recycled as a vertex buffer. The driver determines the number
 *    of primitives, in part, by how much data was written to the
 *    buffer through stream output.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawAuto(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_draw_info info;
   struct pipe_draw_indirect_info indirect;


   if (!pDevice->draw_so_target) {
      LOG_UNSUPPORTED("DrawAuto without a set source buffer!");
      return;
   }

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   ResolveState(pDevice);
   if (RunPixelShaderEmulation(pDevice))
      return;

   util_draw_init_info(&info);
   info.mode = pDevice->primitive;
   memset(&indirect, 0, sizeof indirect);
   indirect.count_from_stream_output = pDevice->draw_so_target;

   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, &indirect, NULL, 1);
}
