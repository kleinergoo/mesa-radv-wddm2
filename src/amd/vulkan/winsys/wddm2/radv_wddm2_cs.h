/*
 * Copyright © 2020 Valve Corporation
 *
 * based on amdgpu winsys.
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef RADV_WDDM2_CS_H
#define RADV_WDDM2_CS_H

#include "radv_wddm2_winsys.h"
#include "radv_radeon_winsys.h"

#include <stdint.h>

struct radv_wddm2_queue {
   enum amd_ip_type hw_ip;
   uint32_t context_h;
   uint32_t hw_context_h;
   uint32_t handle;
   uint32_t queue_id; /* KMD identifier */
   struct vk_wddm2_fence vm_fence;
   /* Hardware queue progress fence (fast submission path). */
   uint64_t *hq_progress_fence_cpu;
   uint64_t hq_submit_seq;
};

struct radv_wddm2_ctx {
   struct radv_wddm2_winsys *ws;

   struct radv_wddm2_queue ace_queue;

   struct {
      struct radv_wddm2_queue queue;
      struct vk_wddm2_fence last_submission;
   } per_ip[AMD_NUM_IP_TYPES];
};

static inline struct radv_wddm2_ctx *
radv_wddm2_ctx(struct radeon_winsys_ctx *base)
{
   return (struct radv_wddm2_ctx *)base;
}

void radv_wddm2_cs_init_functions(struct radv_wddm2_winsys *ws);

#endif /* RADV_WDDM2_CS_H */
