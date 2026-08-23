/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 *
 * Minimal declarations required by generated Venus protocol headers when
 * they are used without the Venus ICD ring implementation.
 */

#ifndef YTTRIUM_MINI_VN_RING_H
#define YTTRIUM_MINI_VN_RING_H

#include "vn_cs.h"

struct vn_ring_submit_command {
   struct vn_cs_encoder command;
   struct vn_cs_encoder_buffer buffer;
   size_t reply_size;

   struct vn_cs_decoder reply;
   bool ring_seqno_valid;
   uint32_t ring_seqno;
};

struct vn_ring {
   void *driver;
   bool (*submit_command)(struct vn_ring *vn_ring,
                          struct vn_ring_submit_command *submit);
   struct vn_cs_decoder *(*get_command_reply)(
      struct vn_ring *vn_ring,
      struct vn_ring_submit_command *submit);
   void (*free_command_reply)(struct vn_ring *vn_ring,
                              struct vn_ring_submit_command *submit);
};

bool
yttrium_vn_ring_submit_command(struct vn_ring *vn_ring,
                               struct vn_ring_submit_command *submit);

struct vn_cs_decoder *
yttrium_vn_ring_get_command_reply(struct vn_ring *vn_ring,
                                  struct vn_ring_submit_command *submit);

void
yttrium_vn_ring_free_command_reply(struct vn_ring *vn_ring,
                                   struct vn_ring_submit_command *submit);

static inline struct vn_cs_encoder *
vn_ring_submit_command_init(struct vn_ring *vn_ring,
                            struct vn_ring_submit_command *submit,
                            void *cmd_data,
                            size_t cmd_size,
                            size_t reply_size)
{
   (void)vn_ring;
   submit->buffer = VN_CS_ENCODER_BUFFER_INITIALIZER(cmd_data);
   submit->command = VN_CS_ENCODER_INITIALIZER(&submit->buffer, cmd_size);
   submit->reply_size = reply_size;
   submit->ring_seqno_valid = false;
   submit->ring_seqno = 0;
   submit->reply = VN_CS_DECODER_INITIALIZER(NULL, 0);
   return &submit->command;
}

static inline struct vn_cs_decoder *
vn_ring_get_command_reply(struct vn_ring *vn_ring,
                          struct vn_ring_submit_command *submit)
{
   return vn_ring && vn_ring->get_command_reply ?
      vn_ring->get_command_reply(vn_ring, submit) : NULL;
}

static inline void
vn_ring_free_command_reply(struct vn_ring *vn_ring,
                           struct vn_ring_submit_command *submit)
{
   if (vn_ring && vn_ring->free_command_reply)
      vn_ring->free_command_reply(vn_ring, submit);
}

static inline void
vn_ring_submit_command(struct vn_ring *vn_ring,
                       struct vn_ring_submit_command *submit)
{
   submit->ring_seqno_valid = vn_ring && vn_ring->submit_command &&
      vn_ring->submit_command(vn_ring, submit);
}

#endif /* YTTRIUM_MINI_VN_RING_H */
