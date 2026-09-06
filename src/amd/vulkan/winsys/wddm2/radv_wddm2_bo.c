/*
 * Copyright © 2020 Valve Corporation
 *
 * based on amdgpu winsys.
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2022 Collabora, Ltd
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

/* Windows headers conflict with Wayland and XLib headers */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "radv_wddm2_bo.h"
#include "radv_wddm2_cs.h"
#include "util/u_memory.h"
#include "vk_async_event.h"
#include "vk_device.h"

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "wsl/winadapter.h"
#endif
#include "d3dkmthk.h"

static const bool all_resident = true;

/* Wait for a paging operation (map / make-resident) to reach `value` on the
 * paging queue's fence WITHOUT blocking the calling thread on a raw kernel
 * wait.  Arming the wait against an async event makes D3DKMT
 * WaitForSynchronizationObjectFromCpu register the wait and return
 * immediately; we then await the event.  Under memory pressure the paging
 * queue can take a long time and a blocking wait object here both freezes the
 * allocating thread and can't be interrupted, so this is the interruptible
 * form the rest of the backend already uses for monitor fences. */
static VkResult
radv_wddm2_wait_paging_fence(struct radv_wddm2_winsys *ws, uint64_t value)
{
   HANDLE async_event = 0;
   VkResult result = vk_async_event_create(&async_event);
   if (unlikely(result != VK_SUCCESS))
      return result;

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &value,
      .hAsyncEvent = async_event,
   };
   NTSTATUS status = WDDM2_DISPATCH(WaitForSynchronizationObjectFromCpu(&wait));
   if (unlikely(!NT_SUCCESS(status))) {
      vk_async_event_close(async_event);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   /* The paging fence is a monitored fence: block on the shared event but keep
    * polling is unnecessary -- the event is authoritative here.  Wait with an
    * effectively infinite timeout; a paging stall eventually resolves or the
    * device is lost (surfacing via the next device-status check). */
   result = vk_async_event_wait(async_event, UINT64_MAX);
   vk_async_event_close(async_event);

   return result;
}

#pragma pack(push, 4)
struct create_alloc_pdata {
   uint32_t adapter_id;
   uint32_t _dw1;
   uint32_t flags; // 0x80
   uint32_t checksum;
   uint32_t reserved[11];
   uint32_t pdata_size;
};

struct alloc_header {
   uint32_t entry_size[4];
   uint32_t adapter_id;
   uint32_t checksum;
   uint32_t reserved[9];
   uint32_t num_entries;
};

struct alloc_bo_info {
   uint32_t section_size;
   uint32_t _dw1;
   uint32_t flags;
   uint32_t size;
   uint32_t alignment;
   uint32_t priority;
   uint8_t heaps[4];
   uint32_t _unknown[16];
   uint32_t flags2;
   uint64_t phys_size;
   uint64_t va_addr;
   uint64_t va_size;
   uint32_t reserved[2];
};

struct alloc_surf {
   uint32_t section_size;
   uint32_t flags;
   uint32_t swizzle_mode; // linear = 0x20
   uint32_t resource_type;
   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t width_in_texels;
   uint32_t height_in_texels;
   uint32_t depth;
   uint32_t slice_size;
   uint32_t _unknown0[16];
   uint32_t width2;
   uint32_t height2;
   uint32_t depth2;
   uint32_t size;
   uint32_t _unknown1[41];
};

struct alloc_metadata  {
   uint32_t section_size;
   uint32_t _pad;
   uint64_t va_addr;
   uint64_t va_size;
   uint32_t _unknown0[45];
   uint32_t mtype;
   uint32_t flags;
   uint32_t mall_policy;
   uint32_t mall_range[2];
   uint32_t _unknown2[13];
};

struct alloc_entry {
   uint32_t entry_size;
   uint32_t enabled_sections;
   struct alloc_bo_info bo_info;
   uint32_t num_planes;
   uint32_t _unknown0[2];
   uint16_t samples;
   uint16_t mask;
   uint32_t mip_levels;
   uint32_t layers;
   uint32_t _unknown1[4];
   uint32_t metadata_offset;
   uint32_t _unknown2[2];
   uint32_t version;
   uint32_t _unknown3;
};
#pragma pack(pop)

static inline uint32_t
calculate_checksum(const uint32_t *data, size_t dword_count)
{
   uint32_t acc[8] = {0};

   for (size_t i = 0; i < dword_count; i++)
      acc[i % 8] += i ^ data[i];
   return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

enum alloc_entry_section {
   ALLOC_SECTION_METADATA = 0x1,
   ALLOC_SECTION_SURF = 0xc,
};

enum alloc_flags {
   ALLOC_FLAG_NOT_VIRTUAL = 0x2000,
   ALLOC_FLAG_UDMA_BUFFER = 0x40000,
   ALLOC_FLAG_HOST_ALLOCATED = 0x4000000,
   ALLOC_FLAG_CPU_VISIBLE = 0x20000000,
   ALLOC_FLAG_SHARED = 0x80000000,
};

enum alloc_heap {
   ALLOC_HEAP_LOCAL = 0,
   ALLOC_HEAP_INVISIBLE = 1,
   ALLOC_HEAP_GART_USWC = 2,
   ALLOC_HEAP_GART_CACHEABLE = 3,
};

#define ADD_HEAP(heap) \
   do { \
      bo_info->flags |= 1 << ALLOC_HEAP_##heap; \
      bo_info->heaps[heap_count++] = ALLOC_HEAP_##heap + 1; \
   } while (0)

static void
fill_alloc_heaps(struct radv_wddm2_winsys *ws, enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                 bool host_allocated, bool gtt_spill, struct alloc_bo_info *bo_info)
{
   uint32_t heap_count = 0;

   /* When gtt_spill is set the VRAM heaps would exceed the WDDM residency
    * budget; back the allocation with GART (system memory) instead so the
    * application does not hard-fail with OUT_OF_DEVICE_MEMORY.  The BO's
    * logical domain stays VRAM; only the physical backing changes.
    */
   if (!gtt_spill && (initial_domain & RADEON_DOMAIN_VRAM)) {
      assert(!host_allocated);
      if (!(flags & RADEON_FLAG_CPU_ACCESS))
         ADD_HEAP(INVISIBLE);
      if (!(flags & RADEON_FLAG_NO_CPU_ACCESS))
         ADD_HEAP(LOCAL);
   }

   if (gtt_spill || (initial_domain & RADEON_DOMAIN_GTT) || initial_domain == 0) {
      if (!(flags & RADEON_FLAG_GTT_WC))
         ADD_HEAP(GART_CACHEABLE);
      if (!host_allocated)
         ADD_HEAP(GART_USWC);
   }
}

#define ADD_SECTION(section) \
   do { \
      section = (struct alloc_##section *)pdata; \
      pdata += sizeof(struct alloc_##section); \
      entry_size += sizeof(struct alloc_##section); \
   } while (0)

#define ADD_OPT_SECTION(section, type) \
   do { \
      entry->enabled_sections |= ALLOC_SECTION_##type; \
      ADD_SECTION(section); \
      section->section_size = sizeof(struct alloc_##section); \
   } while (0)

static uint32_t
fill_alloc_pdata(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment,
                 enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                 unsigned priority, uint64_t address, void *cpu_ptr, bool gtt_spill,
                 uint8_t *pdata)
{
   struct alloc_header *header = (struct alloc_header *)pdata;
   struct alloc_entry *entry;
   struct alloc_surf *surf;
   struct alloc_metadata *metadata;
   uint32_t entry_size = 0;

   pdata += sizeof(struct alloc_header);
   ADD_SECTION(entry);

   // BO info
   entry->bo_info.section_size = sizeof(struct alloc_bo_info);
   entry->bo_info.flags = (flags & RADEON_FLAG_VIRTUAL) ? 0 : ALLOC_FLAG_NOT_VIRTUAL;
   if (!(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING))
      entry->bo_info.flags |= ALLOC_FLAG_SHARED;
   if (!(flags & RADEON_FLAG_NO_CPU_ACCESS))
      entry->bo_info.flags |= ALLOC_FLAG_CPU_VISIBLE;
   if (cpu_ptr)
      entry->bo_info.flags |= ALLOC_FLAG_HOST_ALLOCATED;
   entry->bo_info.size = size;
   entry->bo_info.alignment = alignment;
   entry->bo_info.priority = 0x5;
   if (cpu_ptr)
      entry->bo_info.priority |= 0x800;
   entry->bo_info.phys_size = size;
   entry->bo_info.va_size = size;
   entry->bo_info.va_addr = address;
   entry->bo_info.flags2 = 0x1000000;
   fill_alloc_heaps(ws, initial_domain, flags, cpu_ptr != NULL, gtt_spill, &entry->bo_info);

   if (!(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING)) {
      entry->num_planes = 1;
      entry->mip_levels = 1;
      entry->layers = 1;
      entry->samples = 1;

      ADD_OPT_SECTION(surf, SURF);
      surf->flags = 0x20000; // typed?
      surf->swizzle_mode = 0x20; // linear
      surf->resource_type = 1; // 1D
      surf->width = size;
      surf->height = 1;
      surf->width_in_texels = size;
      surf->height_in_texels = 1;
      surf->depth = 1;
      surf->slice_size = size;
      surf->width2 = size;
      surf->height2 = 1;
      surf->depth2 = 1;
      surf->size = size;
   }

   // Metadata
   entry->metadata_offset = entry_size;
   ADD_OPT_SECTION(metadata, METADATA);
   metadata->va_addr = address;
   metadata->va_size = size;
   if (flags & RADEON_FLAG_NO_CPU_ACCESS)
      metadata->flags |= 0x2000; // no CPU access
   if (flags & RADEON_FLAG_GL2_BYPASS) {
      metadata->mtype = 0x4; // L2_UNCACHED;
      metadata->mall_policy = 0x1; // never
   }

   entry->version = 0x9;
   entry->entry_size = entry_size;

   header->entry_size[0] = entry_size;
   header->adapter_id = 0;
   header->num_entries = 1;
   header->checksum = calculate_checksum((const uint32_t *)header, sizeof(*header) / 4);

   return entry_size + sizeof(*header);
}

static uint64_t
radv_wddm2_get_optimal_vm_alignment(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment)
{
   uint64_t vm_alignment = alignment;

   /* Increase the VM alignment for faster address translation. */
   if (size >= ws->gpu_info.pte_fragment_size)
      vm_alignment = MAX2(vm_alignment, ws->gpu_info.pte_fragment_size);

   /* Gfx9: Increase the VM alignment to the most significant bit set
    * in the size for faster address translation.
    */
   if (ws->gpu_info.gfx_level >= GFX9) {
      unsigned msb = util_last_bit64(size); /* 0 = no bit is set */
      uint64_t msb_alignment = msb ? 1ull << (msb - 1) : 0;

      vm_alignment = MAX2(vm_alignment, msb_alignment);
   }
   return vm_alignment;
}

static uint64_t
radv_wddm2_reserve_va_range(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment,
                            enum radeon_bo_flag flags)
{
   D3DGPU_VIRTUAL_ADDRESS min, max;
   NTSTATUS status;

   /* Keep the 32-bit and replayable heaps in their dedicated windows; give
    * everything else the full [HEAP_START, REPLAY_HEAP_START) range so a
    * sparse/virtual allocation stream cannot exhaust a narrow 4 GB window
    * (DOOM: The Dark Ages streaming reserves several GB of virtual VA). */
   if (flags & RADEON_FLAG_32BIT) {
      min = RADV_WDDM2_32BIT_HEAP_START;
      max = RADV_WDDM2_HEAP_START;
   } else if (flags & RADEON_FLAG_REPLAYABLE) {
      min = RADV_WDDM2_REPLAY_HEAP_START;
      max = RADV_WDDM2_REPLAY_HEAP_START + (4ull << 32);
   } else {
      min = RADV_WDDM2_HEAP_START;
      max = RADV_WDDM2_REPLAY_HEAP_START;
   }

   D3DDDI_RESERVEGPUVIRTUALADDRESS reserve = {
      .hPagingQueue = ws->paging_queue_h,
      .MinimumAddress = min,
      .MaximumAddress = max,
      .Size = size,
   };
   status = WDDM2_DISPATCH(ReserveGpuVirtualAddress(&reserve));
   if (!NT_SUCCESS(status))
      return 0;

   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .BaseAddress = reserve.VirtualAddress,
      .SizeInPages = size / 4096,
      .Protection = {
         .Zero = 1,
      },
   };
   status = WDDM2_DISPATCH(MapGpuVirtualAddress(&map));
   if (!NT_SUCCESS(status))
      return 0;

   return reserve.VirtualAddress;
}

static VkResult
radv_wddm2_virtual_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                             enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                             unsigned priority, uint64_t address, struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   VkResult result;

   /* Courtesy for users using NULL to check if they need to destroy the BO. */
   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;
   bo->base.is_virtual = true;
   bo->base.va = radv_wddm2_reserve_va_range(ws, size, alignment, flags);
   if (bo->base.va == 0) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_reserve;
   }

   *out_bo = &bo->base;
   return VK_SUCCESS;

error_va_reserve:
   FREE(bo);
   return result;
}

/* Track live device-memory allocations in-process so the Vulkan heap-budget
 * math (which mirrors the amdgpu/Linux winsys) reports a stable budget instead
 * of collapsing when WDDM2 uses a stale CurrentReservation == 0. */
static void
radv_wddm2_bo_account(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo, int delta)
{
   if (bo->base.is_virtual)
      return;

   const int64_t d = delta;
   simple_mtx_lock(&ws->alloc_mtx);
   if (bo->base.initial_domain & RADEON_DOMAIN_VRAM) {
      if (bo->flags & RADEON_FLAG_CPU_ACCESS)
         ws->alloc_vram_vis += d;
      else
         ws->alloc_vram += d;
   } else {
      ws->alloc_gtt += d;
   }
   simple_mtx_unlock(&ws->alloc_mtx);
}

static VkResult
radv_wddm2_bo_create_internal(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                              enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                              unsigned priority, uint64_t address, void *cpu_ptr,
                              struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   NTSTATUS status;
   VkResult result;

   /* Courtesy for users using NULL to check if they need to destroy the BO. */
   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;

   uint32_t phys_alignment = MAX2(alignment, 0x1000);
   if (initial_domain & RADEON_DOMAIN_VRAM) {
      if (size >= 0x10000)
         phys_alignment = MAX2(phys_alignment, 0x10000);
      if (size >= 0x40000)
         phys_alignment = MAX2(phys_alignment, 0x40000);
   }

   uint32_t virt_alignment = phys_alignment;
   if (size >= ws->gpu_info.pte_fragment_size)
      virt_alignment = MAX2(virt_alignment, ws->gpu_info.pte_fragment_size);
   const uint64_t phys_size = align64(size, phys_alignment);

   /* Out-of-VRAM spill support: if the KMD cannot place a VRAM allocation within
    * the device-local residency budget (DOOM: The Dark Ages + emulate_rt on the
    * 8GB RX 580), retry the allocation backed by GART (system memory) so the
    * application gets an OUT_OF_DEVICE_MEMORY-free allocation instead of crashing.
    */
   bool gtt_spill = false;
   bool want_vram = (initial_domain & RADEON_DOMAIN_VRAM) != 0;

   simple_mtx_lock(&ws->d3d_mtx);
retry_alloc:
   {
   uint8_t alloc_pdata[824] = {0};
   uint32_t pdata_size;
   pdata_size = fill_alloc_pdata(ws, phys_size, phys_alignment, initial_domain,
                                 flags, priority, address, cpu_ptr, gtt_spill, alloc_pdata);

   D3DDDI_ALLOCATIONINFO2 alloc_info = {
      .pSystemMem = cpu_ptr,
      .pPrivateDriverData = alloc_pdata,
      .PrivateDriverDataSize = pdata_size,
      .VidPnSourceId = 0xffffffff,
      .Priority = D3DDDI_ALLOCATIONPRIORITY_NORMAL,
   };

   struct create_alloc_pdata create_pdata = {
      .adapter_id = 0,
      .flags = 0x80,
      .pdata_size = pdata_size,
   };
   create_pdata.checksum =
      calculate_checksum((const uint32_t *)&create_pdata, sizeof(create_pdata) / 4);

   D3DKMT_CREATEALLOCATION create = {
      .hDevice = ws->device_h,
      .pPrivateDriverData = &create_pdata,
      .PrivateDriverDataSize = sizeof(create_pdata),
      .NumAllocations = 1,
      .pAllocationInfo2 = &alloc_info,
      .Flags = {
         .CreateResource = 1,
         .CreateShared = !(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING),
         .NonSecure = 1,
      }
   };

   status = WDDM2_DISPATCH(CreateAllocation2(&create));
   if (!NT_SUCCESS(status)) {
      static LONG diag_once = 0;
      if (InterlockedCompareExchange(&diag_once, 1, 0) == 0) {
         D3DKMT_GETDEVICESTATE exst = { .hDevice = ws->device_h, .StateType = D3DKMT_DEVICESTATE_EXECUTION };
         NTSTATUS exst_status = WDDM2_DISPATCH(GetDeviceState(&exst));
         fprintf(stderr, "DIAG CreateAllocation2 first-fail 0x%X exec_state=0x%X getdevicestate=0x%X\n",
                 status,
                 NT_SUCCESS(exst_status) ? exst.ExecutionState : 0xffffffffu,
                 NT_SUCCESS(exst_status) ? 0u : (uint32_t)exst_status);
         if (NT_SUCCESS(exst_status) && exst.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT) {
             D3DKMT_GETDEVICESTATE pfst = { .hDevice = ws->device_h, .StateType = D3DKMT_DEVICESTATE_PAGE_FAULT };
             if (NT_SUCCESS(WDDM2_DISPATCH(GetDeviceState(&pfst))))
                fprintf(stderr, "DIAG faulted VA: 0x%" PRIx64 " err=0x%x flags=%u stage=%u\n",
                        pfst.PageFaultState.FaultedVirtualAddress,
                        pfst.PageFaultState.FaultErrorCode.GeneralErrorCode,
                        pfst.PageFaultState.PageFaultFlags,
                        pfst.PageFaultState.FaultedPipelineStage);
          }
       }
       if (want_vram && !gtt_spill) {
         fprintf(stderr, "CreateAllocation2 failed 0x%X (VRAM), spilling to GTT\n", status);
         gtt_spill = true;
         goto retry_alloc;
      }
      fprintf(stderr,
              "CreateAllocation2 failed 0x%X\n"
              "  RADV_WDDM2_BO_DBG size=0x%" PRIx64 " phys_size=0x%" PRIx64
              " phys_align=0x%x virt_align=0x%x\n"
              "  RADV_WDDM2_BO_DBG dom=0x%x flags=0x%x prio=%u addr=0x%" PRIx64
              " cpu_ptr=%p gtt_spill=%d\n"
              "  RADV_WDDM2_BO_DBG pdata_size=%u create_flags res=%d shrd=%d nonsec=%d\n"
              "  RADV_WDDM2_BO_DBG alloc flags=0x%x heaps[0]=0x%x\n",
              status, size, phys_size, phys_alignment, (uint32_t)virt_alignment,
              (unsigned)initial_domain, (unsigned)flags, priority, address, cpu_ptr, gtt_spill ? 1 : 0,
              pdata_size, create.Flags.CreateResource, create.Flags.CreateShared,
              create.Flags.NonSecure, ((struct alloc_entry *)(alloc_pdata + sizeof(struct alloc_header)))->bo_info.flags,
              ((struct alloc_entry *)(alloc_pdata + sizeof(struct alloc_header)))->bo_info.flags & 0xF);
      if (ws->dbg) {
         simple_mtx_lock(&ws->alloc_mtx);
         fprintf(stderr, "  RADV_WDDM2_BO_DBG vram=0x%llx vram_vis=0x%llx gtt=0x%llx deferred=%u\n",
                 (unsigned long long)ws->alloc_vram,
                 (unsigned long long)ws->alloc_vram_vis,
                 (unsigned long long)ws->alloc_gtt,
                 (unsigned)list_length(&ws->deferred_list));
         simple_mtx_unlock(&ws->alloc_mtx);
      }
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_ptr_alloc;
   }

   bo->base.obj_id = alloc_info.hAllocation;
   bo->base.size = phys_size;
   bo->base.handle = alloc_info.hAllocation;

   const D3DKMT_DESTROYALLOCATION2 destroy = {
      .hDevice = ws->device_h,
      .phAllocationList = &bo->base.handle,
      .AllocationCount = 1,
   };

   D3DGPU_VIRTUAL_ADDRESS min = flags & RADEON_FLAG_32BIT ? RADV_WDDM2_32BIT_HEAP_START : RADV_WDDM2_HEAP_START;
   if (flags & RADEON_FLAG_REPLAYABLE)
      min = RADV_WDDM2_REPLAY_HEAP_START;
   D3DGPU_VIRTUAL_ADDRESS max = flags & RADEON_FLAG_32BIT ? RADV_WDDM2_HEAP_START : RADV_WDDM2_REPLAY_HEAP_START;
   if (flags & RADEON_FLAG_REPLAYABLE)
      max = RADV_WDDM2_REPLAY_HEAP_START + (4ull << 32);
   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .BaseAddress = address,
      .MinimumAddress = min,
      .MaximumAddress = max,
      .hAllocation = bo->base.handle,
      .SizeInPages = phys_size / 4096,
      .Protection = {
         .Write = !(flags & RADEON_FLAG_READ_ONLY),
      },
   };
   status = WDDM2_DISPATCH(MapGpuVirtualAddress(&map));
   if (!NT_SUCCESS(status)) {
      simple_mtx_lock(&ws->va_mtx);
      fprintf(stderr,
              "mapping 0x%" PRIx64 " failed: 0x%X\n"
              "  RADV_WDDM2_VA_DBG size=0x%" PRIx64 " live=0x%" PRIx64
              " mapped_total=0x%" PRIx64 " freed_total=0x%" PRIx64 "\n",
              bo->base.va, status, phys_size, ws->va_live,
              ws->va_mapped_total, ws->va_freed_total);
      simple_mtx_unlock(&ws->va_mtx);
      simple_mtx_lock(&ws->alloc_mtx);
      fprintf(stderr,
              "  RADV_WDDM2_VA_DBG dom=0x%x alloc_vram=0x%" PRIx64
              " alloc_vram_vis=0x%" PRIx64 " alloc_gtt=0x%" PRIx64
              " deferred_items=%u\n",
              (unsigned)initial_domain, ws->alloc_vram, ws->alloc_vram_vis,
              ws->alloc_gtt, (unsigned)list_length(&ws->deferred_list));
      simple_mtx_unlock(&ws->alloc_mtx);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }
   bo->base.va = map.VirtualAddress;

   simple_mtx_lock(&ws->va_mtx);
   ws->va_live += bo->base.size;
   ws->va_mapped_total += bo->base.size;
   simple_mtx_unlock(&ws->va_mtx);

   uint64_t paging_fence_value = map.PagingFenceValue;

   if (all_resident) {
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .MustSucceed = 1,
         },
      };
      status = WDDM2_DISPATCH(MakeResident(&make_resident));
      if (!NT_SUCCESS(status)) {
         if (want_vram && !gtt_spill) {
            fprintf(stderr, "MakeResident failed (VRAM), spilling to GTT\n");
            WDDM2_DISPATCH(DestroyAllocation2(&destroy));
            gtt_spill = true;
            goto retry_alloc;
         }
         fprintf(stderr, "MakeResident failed\n");
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto error_va_alloc;
      }

      paging_fence_value = make_resident.PagingFenceValue;
   }

   result = radv_wddm2_wait_paging_fence(ws, paging_fence_value);
   if (result != VK_SUCCESS)
      goto error_va_alloc;

   if (ws->debug_all_bos)
      radv_winsys_bo_list_add(&ws->global_bo_list, &bo->base);
   if (ws->debug_log_bos)
      radv_winsys_log_bo(&ws->bo_log, &bo->base, false);

   radv_wddm2_bo_account(ws, bo, bo->base.size);

   simple_mtx_unlock(&ws->d3d_mtx);

   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;

error_va_alloc:
   status = WDDM2_DISPATCH(DestroyAllocation2(&destroy));
   assert(NT_SUCCESS(status));

error_ptr_alloc:
   simple_mtx_unlock(&ws->d3d_mtx);
   FREE(bo);
   return result;
   }

   assert(0); /* unreachable */
}

static VkResult
radv_wddm2_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                     enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                     unsigned priority, uint64_t address,
                     struct radeon_winsys_bo **out_bo)
{
   VkResult result;
   if (flags & RADEON_FLAG_VIRTUAL)
      result = radv_wddm2_virtual_bo_create(_ws, size, alignment, initial_domain, flags,
                                            priority, address, out_bo);
   else
      result = radv_wddm2_bo_create_internal(_ws, size, alignment, initial_domain, flags,
                                             priority, address, NULL, out_bo);

   if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
      struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
      static LONG oom_diag_once = 0;
      if (InterlockedCompareExchange(&oom_diag_once, 1, 0) == 0) {
         fprintf(stderr,
                 "RADV_WDDM2_OOM size=0x%" PRIx64 " align=0x%x dom=0x%x flags=0x%x prio=%u addr=0x%" PRIx64 "\n",
                 size, alignment, (unsigned)initial_domain, (unsigned)flags, priority, address);
         simple_mtx_lock(&ws->alloc_mtx);
         fprintf(stderr, "RADV_WDDM2_OOM alloc vram=0x%llx vram_vis=0x%llx gtt=0x%llx\n",
                 (unsigned long long)ws->alloc_vram,
                 (unsigned long long)ws->alloc_vram_vis,
                 (unsigned long long)ws->alloc_gtt);
         simple_mtx_unlock(&ws->alloc_mtx);
      }
   }
   return result;
}

static VkResult
radv_wddm2_bo_from_ptr(struct radeon_winsys *_ws, void *pointer, uint64_t size, unsigned priority,
                       struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   enum radeon_bo_domain initial_domain = RADEON_DOMAIN_GTT;
   enum radeon_bo_flag flags = RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_CPU_ACCESS;
   unsigned alignment = ws->gpu_info.gart_page_size;

   return radv_wddm2_bo_create_internal(_ws, size, alignment, initial_domain, flags, priority, 0, pointer, out_bo);
}

static void
radv_wddm2_bo_get_metadata(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                           struct radeon_bo_metadata *md)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);

   memcpy(md, &bo->md, sizeof(*md));
}

static void
radv_wddm2_bo_set_metadata(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                           struct radeon_bo_metadata *md)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);

   memcpy(&bo->md, md, sizeof(*md));
}

static VkResult
radv_wddm2_bo_from_fd(struct radeon_winsys *_ws, int fd, unsigned priority,
                      struct radeon_winsys_bo **out_bo, uint64_t *alloc_size)
{
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static bool
radv_wddm2_bo_get_fd(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, int *fd)
{
   return false;
}

static bool
radv_wddm2_bo_get_flags_from_fd(struct radeon_winsys *_ws, int fd,
                                enum radeon_bo_domain *domains,
                                enum radeon_bo_flag *flags)
{
   return false;
}

static VkResult
radv_wddm2_bo_from_handle(struct radeon_winsys *_ws, void *handle, unsigned priority,
                          struct radeon_winsys_bo **out_bo, uint64_t *alloc_size)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   NTSTATUS status;
   VkResult result;

   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = RADEON_DOMAIN_VRAM;
   bo->ws = ws;
   bo->flags = 0;

   /* Query resource info to determine private data sizes */
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query_info = {
      .hDevice = ws->device_h,
      .hNtHandle = (HANDLE)handle,
   };
   status = WDDM2_DISPATCH(QueryResourceInfoFromNtHandle(&query_info));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "QueryResourceInfoFromNtHandle failed 0x%X\n", status);
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error_alloc;
   }

   /* Allocate buffer for private driver data.
    * Lay out the four WDDM buffers back-to-back so they do not overlap:
    *   [total private driver data] [resource private driver data]
    *   [private runtime data]      [open allocation info array]
    */
   size_t pd_size = query_info.TotalPrivateDriverDataSize;
   size_t res_size = query_info.ResourcePrivateDriverDataSize;
   size_t rt_size = query_info.PrivateRuntimeDataSize;
   size_t ai_size = query_info.NumAllocations * sizeof(D3DDDI_OPENALLOCATIONINFO2);
   void *pdata = calloc(1, pd_size + res_size + rt_size + ai_size);
   if (!pdata) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto error_alloc;
   }
   void *res_pdata = (uint8_t *)pdata + pd_size;
   void *runtime_data = (uint8_t *)res_pdata + res_size;
   D3DDDI_OPENALLOCATIONINFO2 *alloc_info =
      (D3DDDI_OPENALLOCATIONINFO2 *)((uint8_t *)runtime_data + rt_size);

   D3DKMT_OPENRESOURCEFROMNTHANDLE open_resource = {
      .hDevice = ws->device_h,
      .hNtHandle = (HANDLE)handle,
      .NumAllocations = query_info.NumAllocations,
      .pOpenAllocationInfo2 = alloc_info,
      .TotalPrivateDriverDataBufferSize = query_info.TotalPrivateDriverDataSize,
      .pTotalPrivateDriverDataBuffer = pdata,
      .ResourcePrivateDriverDataSize = query_info.ResourcePrivateDriverDataSize,
      .pResourcePrivateDriverData = res_pdata,
      .PrivateRuntimeDataSize = query_info.PrivateRuntimeDataSize,
      .pPrivateRuntimeData = runtime_data,
   };
   status = WDDM2_DISPATCH(OpenResourceFromNtHandle(&open_resource));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error_import;
   }

   struct alloc_entry *entry = (struct alloc_entry *)((uint8_t *) pdata + sizeof(struct alloc_header));

   bo->base.obj_id = bo->base.handle = alloc_info[0].hAllocation;
   bo->base.size = entry->bo_info.phys_size;

   /* Map the opened allocation into GPU virtual address space */
   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .MinimumAddress = RADV_WDDM2_HEAP_START,
      .MaximumAddress = RADV_WDDM2_REPLAY_HEAP_START,
      .hAllocation = bo->base.handle,
      .SizeInPages = bo->base.size / 4096,
      .Protection = {
         .Write = 1,
      },
   };
   status = WDDM2_DISPATCH(MapGpuVirtualAddress(&map));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_import;
   }
   bo->base.va = map.VirtualAddress;

   if (alloc_size)
      *alloc_size = bo->base.size;

   /* Make the allocation resident */
   D3DDDI_MAKERESIDENT make_resident = {
      .hPagingQueue = ws->paging_queue_h,
      .NumAllocations = 1,
      .AllocationList = &bo->base.handle,
      .Flags = {
         .MustSucceed = 1,
      },
   };
   status = WDDM2_DISPATCH(MakeResident(&make_resident));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_map;
   }

   /* Wait for the paging operation to complete */
   result = radv_wddm2_wait_paging_fence(ws, make_resident.PagingFenceValue);
   if (result != VK_SUCCESS)
      goto error_map;

   free(pdata);
   *out_bo = &bo->base;
   return VK_SUCCESS;

error_map:
   {
      const D3DKMT_FREEGPUVIRTUALADDRESS unmap = {
         .hAdapter = ws->adapter_h,
         .BaseAddress = bo->base.va,
         .Size = bo->base.size,
      };
      WDDM2_DISPATCH(FreeGpuVirtualAddress(&unmap));
   }
error_import:
   {
      const D3DKMT_DESTROYALLOCATION2 destroy = {
         .hDevice = ws->device_h,
         .phAllocationList = &bo->base.handle,
         .AllocationCount = 1,
      };
      WDDM2_DISPATCH(DestroyAllocation2(&destroy));
      free(pdata);
   }
error_alloc:
   FREE(bo);
   return result;
}

static bool
radv_wddm2_bo_get_flags_from_handle(struct radeon_winsys *_ws, void *handle,
                                    enum radeon_bo_domain *domains,
                                    enum radeon_bo_flag *flags)
{
   *domains = RADEON_DOMAIN_VRAM;
   *flags = RADEON_FLAG_CPU_ACCESS;

   return true;
}

static bool
radv_wddm2_bo_wait_for_idle(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   return true;
}

static void *
radv_wddm2_bo_map(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                  bool use_fixed_addr, void *fixed_addr)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   if (bo->map && !use_fixed_addr)
      return bo->map;

   if (bo->flags & RADEON_FLAG_NO_CPU_ACCESS) {
      fprintf(stderr, "attempt to map non-CPU-accessible BO\n");
      return NULL;
   }

   D3DKMT_LOCK2 lock = {
      .hDevice = bo->ws->device_h,
      .hAllocation = bo->base.handle,
   };
   status = WDDM2_DISPATCH(Lock2(&lock));
   if (!NT_SUCCESS(status))
      return NULL;

   /* WDDM2 Lock2 cannot place a mapping at a specific CPU address, so the
    * placed-memory contract (VK_EXT_map_memory_placed / VK_MEMORY_MAP_PLACED_*)
    * can only be satisfied if the kernel happens to return the requested
    * address.  Refuse rather than silently handing back a mapping that violates
    * the placed constraint. */
   if (use_fixed_addr && lock.pData != fixed_addr) {
      D3DKMT_UNLOCK2 unlock = {
         .hDevice = bo->ws->device_h,
         .hAllocation = bo->base.handle,
      };
      WDDM2_DISPATCH(Unlock2(&unlock));
      return NULL;
   }

   bo->map = lock.pData;

   return lock.pData;
}

static void
radv_wddm2_bo_unmap(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, bool replace)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   if (bo->map == NULL)
      return;

   const D3DKMT_UNLOCK2 unlock = {
      .hDevice = bo->ws->device_h,
      .hAllocation = bo->base.handle,
   };
   status = WDDM2_DISPATCH(Unlock2(&unlock));
   assert(NT_SUCCESS(status));
   
   bo->map = NULL;
}

static VkResult
radv_wddm2_bo_make_resident(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                            bool resident)
{
   if (all_resident)
      return VK_SUCCESS;

   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   NTSTATUS status;

   if (resident) {
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .MustSucceed = 1,
         },
      };
      status = WDDM2_DISPATCH(MakeResident(&make_resident));
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      return radv_wddm2_wait_paging_fence(ws, make_resident.PagingFenceValue);
   } else {
      D3DKMT_EVICT evict = {
         .hDevice = ws->device_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags.EvictOnlyIfNecessary = false,
      };
      status = WDDM2_DISPATCH(Evict(&evict));
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   return VK_SUCCESS;
}

static void
radv_wddm2_bo_destroy_now(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *bo)
{
   ASSERTED NTSTATUS status;
   struct radeon_winsys *_ws = &ws->base;
   struct radeon_winsys_bo *_bo = &bo->base;

   simple_mtx_lock(&ws->d3d_mtx);

   if (all_resident && !bo->base.is_virtual) {
      D3DKMT_EVICT evict = {
         .hDevice = ws->device_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags.EvictOnlyIfNecessary = false,
      };
      status = WDDM2_DISPATCH(Evict(&evict));
      if (!NT_SUCCESS(status))
         fprintf(stderr, "radv/wddm2: Evict failed 0x%X\n", status);
   }

   radv_wddm2_bo_unmap(_ws, _bo, false);

   const D3DKMT_FREEGPUVIRTUALADDRESS unmap = {
      .hAdapter = ws->adapter_h,
      .BaseAddress = bo->base.va,
      .Size = bo->base.size,
   };
   status = WDDM2_DISPATCH(FreeGpuVirtualAddress(&unmap));

   simple_mtx_lock(&ws->va_mtx);
   ws->va_live -= bo->base.size;
   ws->va_freed_total += bo->base.size;
   simple_mtx_unlock(&ws->va_mtx);

   if (!bo->base.is_virtual) {
      const D3DKMT_DESTROYALLOCATION2 destroy = {
         .hDevice = ws->device_h,
         .phAllocationList = &bo->base.handle,
         .AllocationCount = 1,
      };
      status = WDDM2_DISPATCH(DestroyAllocation2(&destroy));

      if (ws->debug_all_bos)
         radv_winsys_bo_list_del(&ws->global_bo_list, &bo->base);
      if (ws->debug_log_bos)
         radv_winsys_log_bo(&ws->bo_log, &bo->base, true);
   }

   radv_wddm2_bo_account(ws, bo, -bo->base.size);

   simple_mtx_unlock(&ws->d3d_mtx);

   FREE(bo);
}

static void
radv_wddm2_deferred_dispose(struct radv_wddm2_winsys *ws, struct list_head *done)
{
   struct radv_wddm2_deferred_bo *d, *next;

   /* The slow D3DKMT destroy and VA free are performed outside the mutex.
    * The nodes have already been detached from the live list, so no other
    * drainer can observe or dispose them again: handing ownership to `done`
    * makes the drain atomic with respect to concurrent drainers. */
   LIST_FOR_EACH_ENTRY_SAFE(d, next, done, list) {
      radv_wddm2_bo_destroy_now(ws, d->bo);
      free(d);
   }
}

/* Atomically splice the already-retired deferred BOs onto `done` so they can
 * be destroyed outside the mutex without racing concurrent drainers.
 * `force` ignores the fence and retires every pending BO (winsys teardown). */
static void
radv_wddm2_deferred_collect(struct radv_wddm2_winsys *ws, struct list_head *done,
                            bool force)
{
   simple_mtx_lock(&ws->deferred_mtx);

   struct radv_wddm2_deferred_bo *d, *next;
   LIST_FOR_EACH_ENTRY_SAFE(d, next, &ws->deferred_list, list) {
      if (!force && p_atomic_read(d->fence.value_map) < d->fence.wait_value) {
         break;
      }

      list_del(&d->list);
      list_addtail(&d->list, done);
   }

   simple_mtx_unlock(&ws->deferred_mtx);

   radv_wddm2_deferred_dispose(ws, done);
}

/* Destroy deferred BOs whose tagged fence has already been signaled by the GPU.
 * Called without holding ws->deferred_mtx; takes it internally. */
static void
radv_wddm2_deferred_drain(struct radv_wddm2_winsys *ws)
{
   struct list_head done;
   list_inithead(&done);

   radv_wddm2_deferred_collect(ws, &done, false);
}

/* Monitored-fence destroy notification, wired up by radv_device.  The winsys
 * keeps by-value snapshots of submission fences (last_submission and deferred
 * BO entries) which dereference fence->value_map.  When such a fence is about
 * to be destroyed, DestroySynchronizationObject tears down that CPU mapping,
 * so any later drain would dereference a dangling address.  Wait until the GPU
 * has passed the highest wait point referenced on this fence, then clear every
 * snapshot so dropping the deferral protection is safe. */
void
radv_wddm2_notify_fence_destroyed(void *winsys, struct vk_device *device,
                                  uint32_t handle, uint64_t *value_map)
{
   struct radv_wddm2_winsys *ws = winsys;
   struct vk_wddm2_fence wait = { .handle = handle };
   uint64_t max_wait = 0;
   bool found = false;
   struct list_head done;
   struct radv_wddm2_deferred_bo *d, *tmp;

   if (handle == 0)
      return;

   for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      struct vk_wddm2_fence *last = &ws->last_submission[ip];
      if (last->handle == handle) {
         if (last->wait_value > max_wait)
            max_wait = last->wait_value;
         found = true;
      }
   }

   if (ws->default_ctx) {
      for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
         struct vk_wddm2_fence *last = &ws->default_ctx->per_ip[ip].last_submission;
         if (last->handle == handle) {
            if (last->wait_value > max_wait)
               max_wait = last->wait_value;
            found = true;
         }
      }
   }

   simple_mtx_lock(&ws->deferred_mtx);
   LIST_FOR_EACH_ENTRY_SAFE(d, tmp, &ws->deferred_list, list) {
      if (d->fence.handle == handle) {
         if (d->fence.wait_value > max_wait)
            max_wait = d->fence.wait_value;
         found = true;
      }
   }
   simple_mtx_unlock(&ws->deferred_mtx);

   if (!found)
      return;

   if (max_wait > 0 && value_map) {
      wait.wait_value = max_wait;
      wait.value_map = value_map;
      vk_wddm2_fence_wait(device->wddm2_handle, &wait);
   }

   list_inithead(&done);

   simple_mtx_lock(&ws->deferred_mtx);
   for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      struct vk_wddm2_fence *last = &ws->last_submission[ip];
      if (last->handle == handle) {
         last->handle = 0;
         last->wait_value = 0;
         last->value_map = NULL;
      }
   }

   if (ws->default_ctx) {
      for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
         struct vk_wddm2_fence *last = &ws->default_ctx->per_ip[ip].last_submission;
         if (last->handle == handle) {
            last->handle = 0;
            last->wait_value = 0;
            last->value_map = NULL;
         }
      }
   }

   LIST_FOR_EACH_ENTRY_SAFE(d, tmp, &ws->deferred_list, list) {
      if (d->fence.handle == handle) {
         list_del(&d->list);
         list_addtail(&d->list, &done);
      }
   }
   simple_mtx_unlock(&ws->deferred_mtx);

   radv_wddm2_deferred_dispose(ws, &done);
}

static void
radv_wddm2_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);

   radv_wddm2_deferred_drain(ws);

   /* A chained IB / command buffer BO must not be torn down while a previously
    * submitted command stream may still reference its VA via an INDIRECT_BUFFER
    * chain, otherwise that VA can be reused and the GPU would fault executing
    * stale content (use-after-free -> page fault -> DEVICEEXECUTION_HUNG).
    *
    * The winsys fence snapshot is published by the submit path while holding
    * deferred_mtx across the whole SubmitCommand+Siganl+publish critical
    * section, so a destroy taking the same mutex here is strictly ordered either
    * *before* any IB is handed to the kernel (device idle, safe to free) or
    * *after* the referencing submission's fence has been published (in which
    * case we defer until that fence provably retires).  There is no window where
    * the destroy can observe "idle" between SubmitCommand and the publish. */
   simple_mtx_lock(&ws->deferred_mtx);

   bool in_flight = false;
   struct vk_wddm2_fence snap[AMD_NUM_IP_TYPES];
   for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      snap[ip] = ws->last_submission[ip];
      if (snap[ip].handle != 0 &&
          p_atomic_read(snap[ip].value_map) < snap[ip].wait_value)
         in_flight = true;
   }

   simple_mtx_unlock(&ws->deferred_mtx);

   if (in_flight) {
      /* Pick the first still-in-flight fence as the retire guard: waiting on the
       * last submission of any queue implies every earlier submission on that
       * queue has completed, so the guarded VA can be freed once its value is
       * reached.  A BO must only ever be deferred once; if another (app-level)
       * destroy raced us, the existing node covers the fence already. */
      struct vk_wddm2_fence *guard = NULL;
      for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
         if (snap[ip].handle != 0 &&
             p_atomic_read(snap[ip].value_map) < snap[ip].wait_value) {
            guard = &snap[ip];
            break;
         }
      }

      if (guard) {
         struct radv_wddm2_deferred_bo *d = calloc(1, sizeof(*d));
         if (!d) {
            fprintf(stderr, "radv/wddm2: out of memory allocating deferred BO\n");
            return;
         }

         d->bo = bo;
         d->fence = *guard;

         simple_mtx_lock(&ws->deferred_mtx);
         struct radv_wddm2_deferred_bo *already = NULL, *tmp;
         LIST_FOR_EACH_ENTRY(tmp, &ws->deferred_list, list) {
            if (tmp->bo == bo) {
               already = tmp;
               break;
            }
         }

         if (already) {
            simple_mtx_unlock(&ws->deferred_mtx);
            free(d);
            return;
         }

         list_addtail(&d->list, &ws->deferred_list);
         simple_mtx_unlock(&ws->deferred_mtx);
         return;
      }
   }

   /* Every queue snapshot reads idle, taken under the same mutex that serializes
    * the submit+publish critical section, so no IB referencing this BO can be in
    * the kernel at this moment.  Re-validate the snapshot under the lock (a fence
    * could have been destroyed in the meantime, which would make the local
    * handle/value_map copy stale and the value_map page possibly unmapped), then
    * block on the last-known fence for belt-and-suspenders (all idle here, so
    * the waits are quick polls), then free. */
   simple_mtx_lock(&ws->deferred_mtx);
   for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      if (snap[ip].handle != 0 &&
          (ws->last_submission[ip].handle != snap[ip].handle ||
           ws->last_submission[ip].wait_value != snap[ip].wait_value))
         snap[ip].handle = 0;
   }
   simple_mtx_unlock(&ws->deferred_mtx);

   for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      if (snap[ip].handle != 0)
         vk_wddm2_fence_wait(ws->device_h, &snap[ip]);
   }

   radv_wddm2_bo_destroy_now(ws, bo);
}

/* Retire any deferred BOs whose fence has already been signaled. Called from the
 * submit path after a new fence value is published. */
void
radv_wddm2_bo_deferred_drain(struct radv_wddm2_winsys *ws)
{
   radv_wddm2_deferred_drain(ws);
}

/* Force-destroy every pending deferred BO. Used only at winsys teardown when no
 * GPU work can still be referencing them. */
void
radv_wddm2_bo_destroy_deferred_all(struct radv_wddm2_winsys *ws)
{
   struct list_head done;
   list_inithead(&done);

   radv_wddm2_deferred_collect(ws, &done, true);
}
static VkResult
radv_wddm2_virtual_bo_map(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *parent,
                          uint64_t offset, uint64_t size, struct radv_wddm2_bo *bo,
                          uint64_t bo_offset)
{
   struct radv_wddm2_ctx *ctx = ws->default_ctx;
   enum amd_ip_type ip_type = AMD_IP_GFX;

   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op = {
      .OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT,
      .MapProtect = {
         .BaseAddress = parent->base.va + offset,
         .SizeInBytes = size,
         .hAllocation = bo->base.handle,
         .AllocationOffsetInBytes = bo_offset,
         .AllocationSizeInBytes = size,
         .Protection = {
            .Write = 1,
         },
         .DriverProtection = 1,
      },
   };

   const D3DKMT_UPDATEGPUVIRTUALADDRESS update = {
      .hDevice = ws->device_h,
      .hContext = ctx->per_ip[ip_type].queue.context_h,
      .hFenceObject = ctx->per_ip[ip_type].queue.vm_fence.handle,
      .FenceValue = ctx->per_ip[ip_type].queue.vm_fence.wait_value,
      .Operations = &op,
      .NumOperations = 1,
   };

   NTSTATUS status = WDDM2_DISPATCH(UpdateGpuVirtualAddress(&update));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "mapping 0x%" PRIx64 " failed: 0x%X\n", parent->base.va + offset, status);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   return VK_SUCCESS;
}

static VkResult
radv_wddm2_virtual_bo_unmap(struct radv_wddm2_winsys *ws, struct radv_wddm2_bo *parent,
                            uint64_t offset, uint64_t size)
{
   struct radv_wddm2_ctx *ctx = ws->default_ctx;
   enum amd_ip_type ip_type = AMD_IP_GFX;

   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op = {
      .OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP,
      .Unmap = {
         .BaseAddress = parent->base.va + offset,
         .SizeInBytes = size,
         .Protection = {
            .Zero = 1,
         }
      },
   };

   const D3DKMT_UPDATEGPUVIRTUALADDRESS update = {
      .hDevice = ws->device_h,
      .hContext = ctx->per_ip[ip_type].queue.context_h,
      .hFenceObject = ctx->per_ip[ip_type].queue.vm_fence.handle,
      .FenceValue = ctx->per_ip[ip_type].queue.vm_fence.wait_value,
      .Operations = &op,
      .NumOperations = 1,
   };

   NTSTATUS status = WDDM2_DISPATCH(UpdateGpuVirtualAddress(&update));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "mapping 0x%" PRIx64 " failed: 0x%X\n", parent->base.va + offset, status);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   return VK_SUCCESS;
}

static VkResult
radv_wddm2_bo_virtual_bind(struct radeon_winsys *_ws, struct radeon_winsys_bo *_parent, uint64_t offset,
                           uint64_t size, struct radeon_winsys_bo *_bo, uint64_t bo_offset)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *parent = (struct radv_wddm2_bo *)_parent;
   struct radv_wddm2_bo *bo = (struct radv_wddm2_bo *)_bo;
   VkResult ret;

   assert(parent->base.is_virtual);
   assert(!bo || !bo->base.is_virtual);

   if (!ws->default_ctx)
      return VK_ERROR_DEVICE_LOST;

   if (bo) {
      ret = radv_wddm2_virtual_bo_map(ws, parent, offset, size, bo, bo_offset);
   } else {
      ret = radv_wddm2_virtual_bo_unmap(ws, parent, offset, size);
   }

   return ret;
}

static void
radv_wddm2_dump_bo_log(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (!ws->debug_log_bos)
      return;

   radv_winsys_dump_bo_log(&ws->bo_log, file);
}

static void
radv_wddm2_dump_bo_ranges(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (ws->debug_all_bos)
      radv_winsys_dump_bo_ranges(&ws->global_bo_list, file);
   else
      fprintf(file, "  To get BO VA ranges, please specify RADV_DEBUG=allbos\n");
}

void
radv_wddm2_bo_init_functions(struct radv_wddm2_winsys *ws)
{
   ws->base.buffer_create = radv_wddm2_bo_create;
   ws->base.buffer_destroy = radv_wddm2_bo_destroy;
   ws->base.buffer_map = radv_wddm2_bo_map;
   ws->base.buffer_unmap = radv_wddm2_bo_unmap;
   ws->base.buffer_make_resident = radv_wddm2_bo_make_resident;
   ws->base.buffer_from_ptr = radv_wddm2_bo_from_ptr;
   ws->base.buffer_from_fd = radv_wddm2_bo_from_fd;
   ws->base.buffer_get_fd = radv_wddm2_bo_get_fd;
   ws->base.buffer_from_handle = radv_wddm2_bo_from_handle;
   ws->base.buffer_get_flags_from_handle = radv_wddm2_bo_get_flags_from_handle;
   ws->base.buffer_get_flags_from_fd = radv_wddm2_bo_get_flags_from_fd;
   ws->base.buffer_set_metadata = radv_wddm2_bo_set_metadata;
   ws->base.buffer_get_metadata = radv_wddm2_bo_get_metadata;
   ws->base.buffer_virtual_bind = radv_wddm2_bo_virtual_bind;
   ws->base.bo_wait_for_idle = radv_wddm2_bo_wait_for_idle;
   ws->base.dump_bo_ranges = radv_wddm2_dump_bo_ranges;
   ws->base.dump_bo_log = radv_wddm2_dump_bo_log;
}
