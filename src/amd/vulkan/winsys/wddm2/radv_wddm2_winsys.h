/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * based on amdgpu winsys.
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
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

#ifndef RADV_WDDM2_WINSYS_H
#define RADV_WDDM2_WINSYS_H

#include "util/list.h"
#include "util/simple_mtx.h"
#include "vk_wddm2_dispatch_table.h"
#include "ac_gpu_info.h"
#include "radv_winsys_bo.h"
#include "radv_radeon_winsys.h"
#include "vk_sync_binary.h"

struct vk_sync_type;

struct vk_wddm2_fence {
   uint32_t handle;
   uint64_t wait_value;
   uint64_t *value_map;
};

/* A BO whose real destroy (VA free / allocation destroy) has been deferred so a
 * still-in-flight submit that references its VA via an INDIRECT_BUFFER chain can
 * never dangle into a reused address. Freed once the tagged fence retires. */
struct radv_wddm2_deferred_bo {
   struct list_head list;
   struct vk_wddm2_fence fence;
   struct radv_wddm2_bo *bo;
};

struct radv_wddm2_winsys {
   struct radeon_winsys base;

   uint32_t refcount;

   struct radeon_info gpu_info;

   bool debug_all_bos;
   bool debug_log_bos;
   bool chain_ib;
   bool dump_ibs;

   uint32_t adapter_h;
   LUID adapter_luid;
   uint32_t device_h;
   uint32_t paging_queue_h;
   uint32_t paging_fence_h;

   struct radv_winsys_bo_list global_bo_list;
   struct radv_winsys_bo_log bo_log;

   /* In-process device memory accounting.  Reported as RADEON_ALLOCATED_*
    * so that the Vulkan heap-budget math (radv_get_memory_budget_properties)
    * stays flat like the amdgpu/Linux winsys instead of collapsing to the
    * WDDM2 "CurrentReservation == 0" value. */
   simple_mtx_t alloc_mtx;
   uint64_t alloc_vram;
   uint64_t alloc_vram_vis;
   uint64_t alloc_gtt;

   simple_mtx_t deferred_mtx;
   struct list_head deferred_list;

   struct vk_wddm2_fence last_submission[AMD_NUM_IP_TYPES];

   struct vk_sync_binary_type sync_binary_type;
   const struct vk_sync_type *sync_types[3];
   struct {
      void *d3d12_device; 
      void *d3d12_queue;
   } wsi;
};

static inline struct radv_wddm2_winsys *
radv_wddm2_winsys(struct radeon_winsys *base)
{
   return (struct radv_wddm2_winsys *)base;
}

#endif /* RADV_WDDM2_WINSYS_H */
