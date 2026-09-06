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
struct radv_wddm2_ctx;

struct vk_wddm2_fence {
   uint32_t handle;
   uint64_t wait_value;
   uint64_t *value_map;
};

bool vk_wddm2_fence_wait(uint32_t device_h, struct vk_wddm2_fence *fence);

/* Last-N submitted IB snapshot (probe only, see radv_wddm2_cs.c submit path).
 * Kernels are copied verbatim at submit time so a later device-hang DIAG can
 * dump the IB that was executing. */
struct radv_wddm2_last_ib {
   uint32_t ip;
   uint64_t ib_va;
   uint32_t cdw;
   uint32_t ib_buf[4096];
};

struct vk_device;
void radv_wddm2_notify_fence_destroyed(void *winsys, struct vk_device *device,
                                       uint32_t handle, uint64_t *value_map);

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
   bool dbg;                    /* cached RADV_WDDM2_DBG env var */

   uint32_t adapter_h;
   LUID adapter_luid;
   uint32_t device_h;
   uint32_t paging_queue_h;
   uint32_t paging_fence_h;

   /* GPU-alive watchdog: polls GetDeviceState every 200ms. If the engine is
    * HUNG or RESET, dumps execution/page-fault state and the last submitted
    * IB per engine, then kills the process so DWM doesn't freeze. */
   HANDLE watchdog_thread;
   bool watchdog_stop;

   /* Last-submitted IB summary per IP, published under deferred_mtx in the
    * submit critical section. Read by the watchdog on HUNG/RESET to identify
    * the command stream the engine was executing. */
   struct {
      uint64_t va;
      uint32_t len;
      uint64_t fence_value;
   } last_ib_summary[AMD_NUM_IP_TYPES];

   /* Copy of the last main GFX command stream (concatenated chained IB
    * dwords), captured at submit time and dumped verbatim by the watchdog on a
    * GPU hang so the offending packets can be decoded.  Cheap memcpy, so it
    * does not perturb the repro the way full IB decoding does. */
   uint32_t hang_capture[4096];
   unsigned hang_capture_dw;
   bool hang_capture_valid;

   /* WDDM node topology, discovered from the adapter's KMD at winsys creation.
    *
    * Engines are grouped into nodes by engine type (see Windows driver docs,
    * "Enumerating GPU Engine Capabilities"): there is exactly one 3-D node per
    * adapter, and AMD compute/ACE queues are exposed as an additional node
    * reported as DXGK_ENGINE_TYPE_OTHER on multi-node parts.  On single-node
    * parts (e.g. Polaris) the 3-D node also hosts the compute/ACE engines.
    *
    * These are stored here so queue creation is portable across GPU
    * generations instead of assuming a fixed node 0 for everything.
    */
   uint32_t node_count;              /* total engine nodes on the adapter */
   uint32_t gfx_node;                /* the 3-D node (always present) */
   uint32_t compute_node;            /* best compute/ACE node (may equal gfx_node) */
   bool has_dedicated_compute_node;  /* true if compute_ace != gfx_node */

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

   simple_mtx_t va_mtx;
   uint64_t va_live;
   uint64_t va_mapped_total;
   uint64_t va_freed_total;

   simple_mtx_t deferred_mtx;
   struct list_head deferred_list;

   /* Global serialization of the D3DKMT-facing GPU path. Held across every
    * command submission so that submissions from different contexts/devices
    * (e.g. two D3D11 devices) can never race each other on the shared device
    * and paging queue. Lock order: gpu_mtx -> deferred_mtx (deferred_mtx
    * holders never take gpu_mtx). */
   simple_mtx_t gpu_mtx;

   simple_mtx_t d3d_mtx;

   struct vk_wddm2_fence last_submission[AMD_NUM_IP_TYPES];

    struct vk_sync_binary_type sync_binary_type;
    const struct vk_sync_type *sync_types[3];
    struct {
       void *d3d12_device; 
       void *d3d12_queue;
    } wsi;

     /* Representative context used for WDDM2 UpdateGpuVirtualAddress (sparse
      * bind/unbind).  The 26.2.2 buffer_virtual_bind interface no longer passes
      * the context/ip-type, so the first radv device context to be created is
      * remembered here. */
     struct radv_wddm2_ctx *default_ctx;
};

static inline struct radv_wddm2_winsys *
radv_wddm2_winsys(struct radeon_winsys *base)
{
   return (struct radv_wddm2_winsys *)base;
}

#endif /* RADV_WDDM2_WINSYS_H */
