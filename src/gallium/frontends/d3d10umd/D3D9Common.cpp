/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 */
#include "D3D9Private.h"

void
D3D9Tracef(const char *format, ...)
{
   char message[512];
   va_list ap;

   if (!yttrium_gdi_debug_get_bool_option("D3D10UMD_D3D9_TRACE", false))
      return;

   va_start(ap, format);
   vsnprintf(message, sizeof(message), format, ap);
   va_end(ap);
   message[sizeof(message) - 1] = '\0';
   yttrium_gdi_user_logf("d3d9ddi: %s", message);
}

void
D3D9Warnf(const char *format, ...)
{
   char message[512];
   va_list ap;

   va_start(ap, format);
   vsnprintf(message, sizeof(message), format, ap);
   va_end(ap);
   message[sizeof(message) - 1] = '\0';
   yttrium_gdi_trace_warnf("d3d9ddi: %s", message);
   yttrium_gdi_user_logf("WARNING: d3d9ddi: %s", message);
}

void
D3D9WarnOncef(volatile LONG *logged, const char *format, ...)
{
   char message[512];
   va_list ap;

   if (InterlockedCompareExchange(logged, 1, 0) != 0)
      return;

   va_start(ap, format);
   vsnprintf(message, sizeof(message), format, ap);
   va_end(ap);
   message[sizeof(message) - 1] = '\0';
   yttrium_gdi_trace_warnf("d3d9ddi: %s", message);
   yttrium_gdi_user_logf("WARNING: d3d9ddi: %s", message);
}

D3D9SubResource *
D3D9GetSubResource(HANDLE resource_handle, UINT subresource_index)
{
   if (!resource_handle)
      return NULL;

   D3D9Resource *resource = D3D9CastResource(resource_handle);
   if (subresource_index >= resource->surf_count)
      return NULL;
   return &resource->surfaces[subresource_index];
}

D3D9Resource *
D3D9CastResource(HANDLE resource)
{
   return (D3D9Resource *)resource;
}

D3D9Object *
D3D9CreateObject(D3D9ObjectKind kind, size_t payload_size,
                 const void *payload)
{
   D3D9Object *object = (D3D9Object *)calloc(1, sizeof(*object) +
                                             payload_size);
   if (!object)
      return NULL;

   object->kind = kind;
   object->size = payload_size;
   if (payload_size && payload)
      memcpy(object->data, payload, payload_size);
   return object;
}

bool
D3D9CheckConstantRange(UINT reg, UINT count, UINT max_count)
{
   return reg <= max_count && count <= max_count - reg;
}

void
D3D9SetIdentityMatrix(D3DMATRIX *matrix)
{
   memset(matrix, 0, sizeof(*matrix));
   matrix->m[0][0] = 1.0f;
   matrix->m[1][1] = 1.0f;
   matrix->m[2][2] = 1.0f;
   matrix->m[3][3] = 1.0f;
}

void
D3D9MatrixMultiply(D3DMATRIX *dst, const D3DMATRIX *a, const D3DMATRIX *b)
{
   D3DMATRIX out;
   for (UINT row = 0; row < 4; ++row) {
      for (UINT col = 0; col < 4; ++col) {
         out.m[row][col] =
            a->m[row][0] * b->m[0][col] +
            a->m[row][1] * b->m[1][col] +
            a->m[row][2] * b->m[2][col] +
            a->m[row][3] * b->m[3][col];
      }
   }
   *dst = out;
}
