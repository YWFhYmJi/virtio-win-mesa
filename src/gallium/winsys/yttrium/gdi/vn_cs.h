/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 *
 * Minimal command-stream glue for Yttrium's direct use of the generated
 * Venus protocol encoders.  This is intentionally not the full Venus ICD
 * vn_cs.h; it only implements pointer-backed encoders/decoders.
 */

#ifndef YTTRIUM_MINI_VN_CS_H
#define YTTRIUM_MINI_VN_CS_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

#ifndef likely
#define likely(x) (x)
#endif
#ifndef unlikely
#define unlikely(x) (x)
#endif

typedef uint64_t vn_object_id;

struct vn_cs_encoder_buffer {
   void *base;
   size_t committed_size;
};

struct vn_cs_encoder {
   struct vn_cs_encoder_buffer *buffers;
   uint32_t buffer_count;
   uint32_t buffer_max;
   size_t total_committed_size;

   void *cur;
   const void *end;
   bool fatal_error;
   size_t fatal_offset;
   size_t fatal_size;
   size_t fatal_available;
};

struct vn_cs_decoder {
   const void *cur;
   const void *end;
   bool fatal_error;
};

#ifndef VN_TRACE_FUNC
#define VN_TRACE_FUNC() ((void)0)
#endif

#define VN_CS_ENCODER_BUFFER_INITIALIZER(storage) \
   (struct vn_cs_encoder_buffer) { .base = (storage) }

#define VN_CS_ENCODER_INITIALIZER_LOCAL(storage, size)                       \
   (struct vn_cs_encoder) {                                                  \
      .buffers = &VN_CS_ENCODER_BUFFER_INITIALIZER(storage),                 \
      .buffer_count = 1,                                                     \
      .buffer_max = 1,                                                       \
      .cur = (storage),                                                      \
      .end = (const char *)(storage) + (size),                               \
   }

#define VN_CS_ENCODER_INITIALIZER(buf, size)                                 \
   (struct vn_cs_encoder) {                                                  \
      .buffers = (buf),                                                      \
      .buffer_count = 1,                                                     \
      .buffer_max = 1,                                                       \
      .cur = (buf)->base,                                                    \
      .end = (const char *)(buf)->base + (size),                             \
   }

#define VN_CS_DECODER_INITIALIZER(storage, size)                             \
   (struct vn_cs_decoder) {                                                  \
      .cur = (storage),                                                      \
      .end = (const char *)(storage) + (size),                               \
   }

static inline bool
vn_cs_encoder_reserve(struct vn_cs_encoder *enc, size_t size)
{
   const size_t available =
      (size_t)((const uint8_t *)enc->end - (const uint8_t *)enc->cur);
   if (size > available) {
      if (!enc->fatal_error) {
         enc->fatal_offset =
            (size_t)((const uint8_t *)enc->cur -
                     (const uint8_t *)enc->buffers[0].base);
         enc->fatal_size = size;
         enc->fatal_available = available;
      }
      enc->fatal_error = true;
      return false;
   }

   return true;
}

static inline void
vn_cs_encoder_write(struct vn_cs_encoder *enc,
                    size_t size,
                    const void *val,
                    size_t val_size)
{
   assert(val_size <= size);
   if (!vn_cs_encoder_reserve(enc, size))
      return;

   memcpy(enc->cur, val, val_size);
   if (size > val_size)
      memset((uint8_t *)enc->cur + val_size, 0, size - val_size);
   enc->cur = (uint8_t *)enc->cur + size;
}

static inline size_t
vn_cs_encoder_get_len(const struct vn_cs_encoder *enc)
{
   if (!enc->buffer_count)
      return 0;

   return (const uint8_t *)enc->cur -
          (const uint8_t *)enc->buffers[0].base;
}

static inline void
vn_cs_decoder_set_fatal(struct vn_cs_decoder *dec)
{
   dec->fatal_error = true;
}

static inline bool
vn_cs_decoder_peek_internal(const struct vn_cs_decoder *dec,
                            size_t size,
                            void *val,
                            size_t val_size)
{
   assert(val_size <= size);
   if (size > (size_t)((const uint8_t *)dec->end - (const uint8_t *)dec->cur)) {
      memset(val, 0, val_size);
      ((struct vn_cs_decoder *)dec)->fatal_error = true;
      return false;
   }

   memcpy(val, dec->cur, val_size);
   return true;
}

static inline void
vn_cs_decoder_read(struct vn_cs_decoder *dec,
                   size_t size,
                   void *val,
                   size_t val_size)
{
   if (vn_cs_decoder_peek_internal(dec, size, val, val_size))
      dec->cur = (const uint8_t *)dec->cur + size;
}

static inline void
vn_cs_decoder_peek(const struct vn_cs_decoder *dec,
                   size_t size,
                   void *val,
                   size_t val_size)
{
   vn_cs_decoder_peek_internal(dec, size, val, val_size);
}

struct yttrium_venus_object_id {
   vn_object_id id;
};

static inline vn_object_id
vn_cs_handle_load_id(const void **handle, VkObjectType type)
{
   (void)type;
   const struct yttrium_venus_object_id *obj =
      handle ? (const struct yttrium_venus_object_id *)*handle : NULL;
   return obj ? obj->id : 0;
}

static inline void
vn_cs_handle_store_id(void **handle, vn_object_id id, VkObjectType type)
{
   (void)type;
   struct yttrium_venus_object_id *obj =
      handle ? (struct yttrium_venus_object_id *)*handle : NULL;
   if (obj)
      obj->id = id;
}

static inline bool
vn_cs_renderer_protocol_has_api_version(uint32_t api_version)
{
   (void)api_version;
   return true;
}

static inline bool
vn_cs_renderer_protocol_has_extension(uint32_t ext_number)
{
   (void)ext_number;
   return true;
}

#endif /* YTTRIUM_MINI_VN_CS_H */
