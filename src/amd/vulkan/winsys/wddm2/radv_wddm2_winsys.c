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

#include <stdint.h>

#include "radv_wddm2_winsys.h"

#include "radv_wddm2_winsys_public.h"
#include "util/macros.h"
#include "util/u_memory.h"
#include "util/u_string.h"
#include "ac_surface.h"
#include "tools/radv_debug.h"
#include "radv_wddm2_bo.h"
#include "radv_wddm2_cs.h"
#include "radv_wddm2_wsi.h"
#include "vk_dxcore.h"
#include "vk_instance.h"
#include "vk_sync_dummy.h"
#include "vk_wddm2_dispatch_table.h"
#include "vk_wddm2_monitored_fence.h"
#include "common/ac_linux_drm.h"
#include "common/amd_family.h"
#include "common/amdgpu_devices.h"

#include <locale.h>

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */
#include "wsl/winadapter.h"
#include "d3dkmthk.h"

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

static simple_mtx_t winsys_creation_mutex = SIMPLE_MTX_INITIALIZER;
static struct hash_table *winsyses = NULL;

/* Key the winsys hash table on the adapter LUID so more than one adapter can
 * be handled (the previous (void *) 1 constant keyed every adapter to the same
 * slot, silently deduplicating distinct adapters).  The 8-byte LUID packs into
 * a single uintptr_t. */
static uintptr_t
winsys_luid_key(const LUID *luid)
{
   return ((uintptr_t)(uint32_t)luid->HighPart << 32) | (uint32_t)luid->LowPart;
}

/* GPU-alive watchdog: polls GetDeviceState every 50ms. On any non-ACTIVE
 * execution state (HUNG / RESET / page fault / DMA fault / out-of-memory /
 * stopped) it first tries to grab the KMD page-fault details (faulted VA,
 * pipeline stage) while they are still available - the device is removed
 * quickly after a fault - then kills the process so DWM doesn't freeze. */
static DWORD WINAPI
radv_wddm2_watchdog_thread(LPVOID param)
{
   struct radv_wddm2_winsys *ws = param;
   while (!ws->watchdog_stop) {
      Sleep(50);
      D3DKMT_GETDEVICESTATE get_state = {
         .hDevice = ws->device_h,
         .StateType = D3DKMT_DEVICESTATE_EXECUTION,
      };
      NTSTATUS status = WDDM2_DISPATCH(GetDeviceState(&get_state));
      if (NT_SUCCESS(status) &&
          get_state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE &&
          get_state.ExecutionState != D3DKMT_DEVICEEXECUTION_STOPPED) {
         fprintf(stderr, "radv/wddm2: watchdog: GPU state=0x%x, dumping device state\n",
                 get_state.ExecutionState);

         /* Query page-fault details too: an unrecoverable fault is frequently
          * what leaves the engine stuck, and the faulted VA / pipeline stage
          * tells us which allocation or engine the GPU died on.  The device is
          * removed shortly after a fault, so grab this before it disappears. */
         D3DKMT_GETDEVICESTATE pfst = {
            .hDevice = ws->device_h,
            .StateType = D3DKMT_DEVICESTATE_PAGE_FAULT,
         };
         status = WDDM2_DISPATCH(GetDeviceState(&pfst));
         if (NT_SUCCESS(status))
            fprintf(stderr,
                    "radv/wddm2: watchdog: PAGE_FAULT va=0x%llx err=0x%x vend=%i flags=0x%x stage=%u\n",
                    (unsigned long long)pfst.PageFaultState.FaultedVirtualAddress,
                    pfst.PageFaultState.FaultErrorCode.GeneralErrorCode,
                    pfst.PageFaultState.FaultErrorCode.DeviceSpecificCode,
                    pfst.PageFaultState.PageFaultFlags,
                    pfst.PageFaultState.FaultedPipelineStage);
         else
            fprintf(stderr, "radv/wddm2: watchdog: page-fault state query returned 0x%X (no fault info)\n",
                    (unsigned)status);

         /* Report the last IB handed to each engine so the offending command
          * stream can be identified. */
         for (unsigned ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
            if (ws->last_ib_summary[ip].va)
               fprintf(stderr,
                       "radv/wddm2: watchdog: last IB ip=%u va=0x%llx len=%u fence_value=%llu\n",
                       ip, (unsigned long long)ws->last_ib_summary[ip].va,
                       ws->last_ib_summary[ip].len,
                       (unsigned long long)ws->last_ib_summary[ip].fence_value);
         }

         /* Dump the captured last main GFX command stream verbatim. */
         if (ws->hang_capture_valid) {
            fprintf(stderr, "radv/wddm2: watchdog: last GFX CS (%u dwords):\n",
                    ws->hang_capture_dw);
            for (unsigned i = 0; i < ws->hang_capture_dw; i++) {
               if (i % 8 == 0)
                  fprintf(stderr, "radv/wddm2:   %04x:", i);
               fprintf(stderr, " %08x", ws->hang_capture[i]);
               if (i % 8 == 7)
                  fprintf(stderr, "\n");
            }
            if (ws->hang_capture_dw % 8)
               fprintf(stderr, "\n");
         }

         TerminateProcess(GetCurrentProcess(), 1);
      }
   }
   return 0;
}

static NTSTATUS
query_adapter_info(struct radv_wddm2_winsys *ws,
                   KMTQUERYADAPTERINFOTYPE info_type,
                   void *info, size_t info_size)
{
   D3DKMT_QUERYADAPTERINFO adapter_info = {
      .hAdapter = ws->adapter_h,
      .Type = info_type,
      .pPrivateDriverData = info,
      .PrivateDriverDataSize = info_size,
   };
   return WDDM2_DISPATCH(QueryAdapterInfo(&adapter_info));
}

static enum amd_ip_type
convert_node_type(char node_type)
{
   switch (node_type) {
   case 0x00:
      return AMD_IP_GFX;
   case 0x04:
      return AMD_IP_COMPUTE;
   case 0x05:
      return AMD_IP_SDMA;
   default:
      UNREACHABLE("Unknown node type");
   }
}

static uint32_t
kmd_to_amdgpu_vram_type(uint32_t kmd_type)
{
   switch (kmd_type) {
   case 1:
      return AMDGPU_VRAM_TYPE_GDDR3;
   case 2:
      return AMDGPU_VRAM_TYPE_GDDR5;
   case 3:
      return AMDGPU_VRAM_TYPE_GDDR4;
   case 4:
      return AMDGPU_VRAM_TYPE_DDR3;
   case 5:
      return AMDGPU_VRAM_TYPE_HBM;
   case 6:
      return AMDGPU_VRAM_TYPE_GDDR6;
   case 7:
      return AMDGPU_VRAM_TYPE_DDR2;
   case 8:
      return AMDGPU_VRAM_TYPE_DDR4;
   case 9:
      return AMDGPU_VRAM_TYPE_DDR5;
   case 10:
      return AMDGPU_VRAM_TYPE_LPDDR4;
   case 11:
      return AMDGPU_VRAM_TYPE_LPDDR5;
   case 12:
      return AMDGPU_VRAM_TYPE_HBM3E;
   case 13:
      return AMDGPU_VRAM_TYPE_HBM4;
   default:
      fprintf(stderr, "Unknown KMD VRAM type: %u\n", kmd_type);
      return AMDGPU_VRAM_TYPE_UNKNOWN;
   }
}

static void
radv_wddm2_fill_default_max_submitted_ibs(struct radeon_info *info)
{
   /* When the number of IBs can't be queried from the kernel, we choose a
    * rough estimate that should work well (as of kernel 6.3).
    */
   for (unsigned i = 0; i < AMD_NUM_IP_TYPES; ++i)
      info->max_submitted_ibs[i] = 50;

   info->max_submitted_ibs[AMD_IP_GFX] = info->gfx_level >= GFX7 ? 192 : 144;
   info->max_submitted_ibs[AMD_IP_COMPUTE] = 124;
   info->max_submitted_ibs[AMD_IP_VCN_JPEG] = 16;
   for (unsigned i = 0; i < AMD_NUM_IP_TYPES; ++i) {
      /* Clear out max submitted IB count for IPs that have no queues. */
      if (!info->ip[i].num_queues)
         info->max_submitted_ibs[i] = 0;
   }
}

static void
radv_wddm2_fill_attribute_ring_info(struct radeon_info *info)
{
   if (info->gfx_level >= GFX11) {
      unsigned num_prim_exports = 0, num_pos_exports = 0;

      if (info->gfx_level >= GFX12) {
         info->attribute_ring_size_per_se = 1400 * 1024;
         num_prim_exports = 16368; /* also includes gs_alloc_req */
         num_pos_exports = 16384;
      } else if (info->l3_cache_size_mb) {
         info->attribute_ring_size_per_se = 1400 * 1024;
      } else {
         assert(info->num_se == 1);

         if (info->l2_cache_size >= 2 * 1024 * 1024)
            info->attribute_ring_size_per_se = 768 * 1024;
         else
            info->attribute_ring_size_per_se = info->l2_cache_size / 2;
      }

      /* The size must be aligned to 64K per SE and must be at most 16M in total. */
      info->attribute_ring_size_per_se = align(info->attribute_ring_size_per_se, 64 * 1024);
      assert(info->attribute_ring_size_per_se * info->max_se <= 16 * 1024 * 1024);

      /* Compute the pos and prim ring sizes and offsets. */
      info->pos_ring_size_per_se = align(num_pos_exports * 16, 32);
      info->prim_ring_size_per_se = align(num_prim_exports * 4, 32);
      assert(info->gfx_level >= GFX12 ||
             (!info->pos_ring_size_per_se && !info->prim_ring_size_per_se));

      uint32_t max_se_squared = info->max_se * info->max_se;
      uint32_t attribute_ring_size = info->attribute_ring_size_per_se * info->max_se;
      uint32_t pos_ring_size = align(info->pos_ring_size_per_se * max_se_squared, 64 * 1024);
      uint32_t prim_ring_size = align(info->prim_ring_size_per_se * max_se_squared, 64 * 1024);

      info->pos_ring_offset = attribute_ring_size;
      info->prim_ring_offset = info->pos_ring_offset + pos_ring_size;
      info->total_attribute_pos_prim_ring_size = info->prim_ring_offset + prim_ring_size;
   }
}

static void
radv_wddm2_fill_scratch_info(struct radeon_info *info)
{
   /* Compute the scratch WAVESIZE granularity in bytes. */
   info->scratch_wavesize_granularity_shift = info->gfx_level >= GFX11 ? 8 : 10;
   info->scratch_wavesize_granularity = BITFIELD_BIT(info->scratch_wavesize_granularity_shift);

   /* The maximum number of scratch waves. The number is only a function of the number of CUs.
    * It should be large enough to hold at least 1 threadgroup. Use the minimum per-SA CU count.
    *
    * We can decrease the number to make it fit into the infinity cache.
    */
   const unsigned max_waves_per_tg = 32; /* 1024 threads in Wave32 */
   info->max_scratch_waves = MAX2(32 * info->max_good_cu_per_sa * info->max_sa_per_se * info->num_se,
                                  max_waves_per_tg);
   info->has_scratch_base_registers = info->gfx_level >= GFX11 ||
                                      (!info->has_graphics && info->family >= CHIP_GFX940);
}

static void
radv_wddm2_fill_raster_config(struct radeon_info *info)
{
   unsigned raster_config, raster_config_1, se_tile_repeat;

   if (info->gfx_level >= GFX9) {
      info->se_tile_repeat = 32 * info->max_se;
      return;
   }

   switch (info->family) {
   /* 1 SE / 1 RB */
   case CHIP_HAINAN:
   case CHIP_KABINI:
   case CHIP_STONEY:
      raster_config = 0x00000000;
      raster_config_1 = 0x00000000;
      break;
   /* 1 SE / 4 RBs */
   case CHIP_VERDE:
      raster_config = 0x0000124a;
      raster_config_1 = 0x00000000;
      break;
   /* 1 SE / 2 RBs (Oland is special) */
   case CHIP_OLAND:
      raster_config = 0x00000082;
      raster_config_1 = 0x00000000;
      break;
   /* 1 SE / 2 RBs */
   case CHIP_KAVERI:
   case CHIP_ICELAND:
   case CHIP_CARRIZO:
      raster_config = 0x00000002;
      raster_config_1 = 0x00000000;
      break;
   /* 2 SEs / 4 RBs */
   case CHIP_BONAIRE:
   case CHIP_POLARIS11:
   case CHIP_POLARIS12:
      raster_config = 0x16000012;
      raster_config_1 = 0x00000000;
      break;
   /* 2 SEs / 8 RBs */
   case CHIP_TAHITI:
   case CHIP_PITCAIRN:
      raster_config = 0x2a00126a;
      raster_config_1 = 0x00000000;
      break;
   /* 4 SEs / 8 RBs */
   case CHIP_TONGA:
   case CHIP_POLARIS10:
      raster_config = 0x16000012;
      raster_config_1 = 0x0000002a;
      break;
   /* 4 SEs / 16 RBs */
   case CHIP_HAWAII:
   case CHIP_FIJI:
   case CHIP_VEGAM:
      raster_config = 0x3a00161a;
      raster_config_1 = 0x0000002e;
      break;
   default:
      fprintf(stderr, "ac: Unknown GPU, using 0 for raster_config\n");
      raster_config = 0x00000000;
      raster_config_1 = 0x00000000;
      break;
   }

   /* drm/radeon on Kaveri is buggy, so disable 1 RB to work around it.
    * This decreases performance by up to 50% when the RB is the bottleneck.
    */
   if (info->family == CHIP_KAVERI && !info->is_amdgpu)
      raster_config = 0x00000000;

   /* Fiji: Old kernels have incorrect tiling config. This decreases
    * RB performance by 25%. (it disables 1 RB in the second packer)
    */
   if (info->family == CHIP_FIJI && info->cik_macrotile_mode_array[0] == 0x000000e8) {
      raster_config = 0x16000012;
      raster_config_1 = 0x0000002a;
   }

   unsigned se_width = 8 << G_028350_SE_XSEL_GFX6(raster_config);
   unsigned se_height = 8 << G_028350_SE_YSEL_GFX6(raster_config);

   /* I don't know how to calculate this, though this is probably a good guess. */
   se_tile_repeat = MAX2(se_width, se_height) * info->max_se;

   info->pa_sc_raster_config = raster_config;
   info->pa_sc_raster_config_1 = raster_config_1;
   info->se_tile_repeat = se_tile_repeat;
}

static NTSTATUS
radv_wddm2_fill_gpu_info(struct radv_wddm2_winsys *ws,
                         const struct vk_dx_adapter_info *adapter_info)
{
   struct radeon_info *info = &ws->gpu_info;
   uint32_t node_count = 0;
   NTSTATUS status;

   struct drm_amdgpu_memory_info mem = {};
   struct drm_amdgpu_info_device dev = {
      .min_engine_clock = UINT64_C(500000),
      .max_engine_clock = UINT64_C(2371000),
      .min_memory_clock = UINT64_C(96000),
      .max_memory_clock = UINT64_C(1249000),
      .gart_page_size = 4096,
      .tcp_cache_size = 32,
   };
   D3DKMT_QUERY_DEVICE_IDS query_ids = {
      .PhysicalAdapterIndex = adapter_info->physical_adapter_index,
   };

   /* PCI device ids */
   {
      status = query_adapter_info(ws, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &query_ids, sizeof(query_ids));
      if (!NT_SUCCESS(status))
         return status;

      dev.device_id = query_ids.DeviceIds.DeviceID;
      dev.pci_rev = query_ids.DeviceIds.RevisionID;
      if (ws->debug_all_bos)
         fprintf(stderr, "phys adapter = %i device = %x rev %x\n", adapter_info->physical_adapter_index, dev.device_id,
                 dev.pci_rev);
   }

   /* PCI bus address */
   {
      D3DKMT_ADAPTERADDRESS address = {};
      if (NT_SUCCESS(query_adapter_info(ws, KMTQAITYPE_ADAPTERADDRESS,
                                        &address, sizeof(address)))) {
         info->pci.domain = 0;
         info->pci.bus = address.BusNumber;
         info->pci.dev = address.DeviceNumber;
         info->pci.func = address.FunctionNumber;
         info->pci.valid = true;
      } else {
         memset(&info->pci, 0, sizeof(info->pci));
      }
   }

   info->valid_luid = true;
   memcpy(info->luid, &ws->adapter_luid, sizeof(info->luid));

   /* GTT and VRAM size */
   {
      D3DKMT_SEGMENTSIZEINFO segment = {};
      if (NT_SUCCESS(query_adapter_info(ws, KMTQAITYPE_GETSEGMENTSIZE,
                                        &segment, sizeof(segment)))) {
         mem.gtt.total_heap_size = segment.SharedSystemMemorySize;
         mem.vram.total_heap_size = segment.DedicatedVideoMemorySize;
         /*
          * Do NOT report the full DedicatedVideoMemory as CPU-accessible here.
          *
          * On a discrete GPU the segment query reports the entire VRAM as CPU
          * accessible, which collapses the hidden-VRAM heap (all_vram_visible=1)
          * and exposes every DEVICE_LOCAL allocation as HOST_VISIBLE. That
          * diverges from the Linux/amdgpu memory model (a small BAR aperture of
          * ~256 MiB visible VRAM + a large hidden VRAM heap) and forces the whole
          * allocation stream through the fragile WDDM2 host-mapping path, which
          * can produce stale/NULL host pointers that corrupt engine control flow.
          *
          * cpu_accessible_vram is set authoritatively below: from the static
          * amdgpu device database when a device match exists, or from the
          * KMD vram_vis_size in the RDNA3 dynamic query fallback.
          */
         mem.cpu_accessible_vram.total_heap_size = 0;
      }
   }

   /* Nodes information
    *
    * Engines on a WDDM adapter are grouped into nodes by engine type; there is
    * exactly one 3-D node per adapter, and AMD compute/ACE queues live either on
    * that 3-D node (single-node parts such as Polaris) or on a separate node
    * reported as DXGK_ENGINE_TYPE_OTHER (multi-node parts such as Vega/Navi).
    *
    * We discover this topology from the KMD and persist it on the winsys so
    * queue creation below is portable across GPU generations.  The env overrides
    * (RADV_DXGI_*_NODE) let the node be pinned for debugging/validation.
    */
   ws->node_count = 0;
   ws->gfx_node = 0;
   info->has_gpuvm_fault_query = true;
   ws->compute_node = 0;
   ws->has_dedicated_compute_node = false;

   D3DKMT_QUERYSTATISTICS stats = {
      .Type = D3DKMT_QUERYSTATISTICS_ADAPTER,
      .AdapterLuid = {
         .LowPart = adapter_info->adapter_luid.LowPart,
         .HighPart = adapter_info->adapter_luid.HighPart,
      },
   };
   if (NT_SUCCESS(WDDM2_DISPATCH(QueryStatistics(&stats))))
      node_count = stats.QueryResult.AdapterInformation.NodeCount;

   {
      char msg[128];
      snprintf(msg, sizeof(msg), "fill_gpu_info: node_count = %u", node_count);
      if (getenv("RADV_WDDM2_TRACE")) {
         fprintf(stderr, "[wddm2probe:winsys] %s\n", msg);
      }
   }

   for (uint32_t i = 0; i < node_count; ++i) {
      D3DKMT_NODEMETADATA metadata = {
         .NodeOrdinalAndAdapterIndex = MAKEWORD(i, adapter_info->physical_adapter_index),
      };
      if (!NT_SUCCESS(query_adapter_info(ws, KMTQAITYPE_NODEMETADATA,
                                         &metadata, sizeof(metadata))))
         continue;

      if (ws->debug_all_bos)
         fprintf(stderr, "node %i (type: %i) = %ls, flags = %x\n",
                 i, metadata.NodeData.EngineType, metadata.NodeData.FriendlyName,
                 metadata.NodeData.Flags.Value);

      /* 3-D node: always present, hosts the graphics + (on single-node parts)
       * the compute/ACE engines. */
      if (metadata.NodeData.EngineType == DXGK_ENGINE_TYPE_3D)
         ws->gfx_node = i;
      /* AMD exposes its compute/ACE engine as a dedicated node reported with the
       * proprietary OTHER engine type (there is no DXGK_ENGINE_TYPE_COMPUTE).
       * Prefer it for compute work; fall back to gfx_node when absent. */
      else if (metadata.NodeData.EngineType == DXGK_ENGINE_TYPE_OTHER)
         ws->compute_node = i;
   }

   ws->node_count = node_count;
   ws->has_dedicated_compute_node = (ws->compute_node != ws->gfx_node &&
                                     ws->compute_node != 0);

   if (ws->debug_all_bos)
      fprintf(stderr, "radv/wddm2: nodes=%u gfx=%u compute=%u dedicated_compute=%d\n",
              ws->node_count, ws->gfx_node, ws->compute_node,
              (int)ws->has_dedicated_compute_node);

   /* Search static device database */
   const struct amdgpu_device *amdgpu_dev = NULL;
   for (size_t i = 0; i < num_amdgpu_devices; i++) {
      if (amdgpu_devices[i].dev.device_id == dev.device_id) {
         amdgpu_dev = &amdgpu_devices[i];
         break;
      }
   }

   struct drm_amdgpu_info_hw_ip hw_ip_gfx = {0};
   struct drm_amdgpu_info_hw_ip hw_ip_compute = {0};
   struct amdgpu_gpu_info amdinfo = {0};
   bool has_tiling_info = false;

   if (amdgpu_dev) {
      if (ws->debug_all_bos)
         fprintf(stderr, "radv/wddm2: Matched static device config for '%s' (0x%04x)\n",
                 amdgpu_dev->name, dev.device_id);
      dev = amdgpu_dev->dev;
      dev.device_id = query_ids.DeviceIds.DeviceID;
      dev.pci_rev = query_ids.DeviceIds.RevisionID;

      hw_ip_gfx = amdgpu_dev->hw_ip_gfx;
      hw_ip_compute = amdgpu_dev->hw_ip_compute;

      info->me_fw_version = amdgpu_dev->fw_gfx_me.ver;
      info->me_fw_feature = amdgpu_dev->fw_gfx_me.feature;
      info->mec_fw_version = amdgpu_dev->fw_gfx_mec.ver;
      info->mec_fw_feature = amdgpu_dev->fw_gfx_mec.feature;
      info->pfp_fw_version = amdgpu_dev->fw_gfx_pfp.ver;
      info->pfp_fw_feature = amdgpu_dev->fw_gfx_pfp.feature;

      if (mem.vram.total_heap_size == 0) {
         mem.vram.total_heap_size = amdgpu_dev->mem.vram.total_heap_size;
      }
      /* The static amdgpu device database carries the authoritative CPU-visible
       * (BAR aperture) VRAM size for this discrete device. Always honor it so
       * the Vulkan memory model matches Linux (hidden VRAM heap + small visible
       * aperture) instead of exposing all dedicated VRAM as host-visible.
       */
      if (amdgpu_dev->mem.cpu_accessible_vram.total_heap_size)
         mem.cpu_accessible_vram.total_heap_size = amdgpu_dev->mem.cpu_accessible_vram.total_heap_size;
      if (mem.gtt.total_heap_size == 0) {
         mem.gtt.total_heap_size = amdgpu_dev->mem.gtt.total_heap_size;
      }

      /* Populate tiling info from mmr_regs */
      amdinfo.family_id = amdgpu_dev->dev.family;
      amdinfo.chip_external_rev = amdgpu_dev->dev.external_rev;
      memcpy(&amdinfo.cu_bitmap[0][0], &amdgpu_dev->dev.cu_bitmap[0][0], sizeof(amdinfo.cu_bitmap));

      for (uint32_t j = 0; j < amdgpu_dev->mmr_reg_count; j++) {
         uint32_t reg = amdgpu_dev->mmr_regs[j * 3];
         uint32_t val = amdgpu_dev->mmr_regs[j * 3 + 2];
         if (reg == 0x263e) {
            amdinfo.gb_addr_cfg = val;
         } else if (reg == 0x9d8) {
            amdinfo.mc_arb_ramcfg = val;
         } else if (reg >= 0x2644 && reg < 0x2644 + 32) {
            amdinfo.gb_tile_mode[reg - 0x2644] = val;
         } else if (reg >= 0x2664 && reg < 0x2664 + 16) {
            amdinfo.gb_macro_tile_mode[reg - 0x2664] = val;
         }
      }
      has_tiling_info = true;
      info->gb_addr_config = amdinfo.gb_addr_cfg;
      info->num_tile_pipes = amdgpu_dev->dev.num_rb_pipes;
   } else {
      /* Fallback to dynamic RDNA3 KMD structure query */
      struct PACKED umdprivatedata_header {
         uint32_t flags[3];
         uint32_t vendor_id;
         uint32_t unknown0;
         uint32_t pci_id;
         uint32_t unknown1[9];
         uint16_t registry_path[100];
         uint32_t padding[490];
         uint32_t type;
         uint32_t something;
         uint32_t unknown2[42];
         uint32_t size;
      };

      struct PACKED basic_properties {
         struct umdprivatedata_header header;
         uint32_t unk0[11];
         uint32_t device_id;
         uint32_t family_id;
         uint32_t chip_external_rev;
         uint32_t subsystem_id;
         uint32_t unk1[6];
         uint32_t vram_type;
         uint32_t vram_bit_width;
         uint32_t unk2[43];
         char umd_registry_path[256];
         uint32_t unk3[18];
         uint32_t num_rb_pipes;
         uint32_t num_enabled_rb_pipes;
         uint32_t enabled_rb_pipes_mask;
         uint32_t enabled_rb_pipes_mask_hi;
         uint32_t unk4[8];
      };

      struct PACKED gpu_properties {
         struct umdprivatedata_header header;
         uint32_t unk0[550];
         uint32_t num_engines;
         char engines[64];
         uint32_t unk1[5];
         uint32_t me_fw_version;
         uint32_t mec_fw_version;
         uint32_t pfp_fw_version;
         uint32_t unk3[65];
         uint32_t device_id;
         uint32_t family_id;
         uint32_t chip_external_rev;
         uint32_t chip_rev;
         uint32_t gfx_engine_id;
         uint32_t unk4[4];
         uint32_t clock_crystal_freq_hz;
         uint32_t unk5;
         uint32_t unk6;
         uint64_t vram_vis_size;
         uint64_t vram_inv_size;
         uint32_t unk7[112];
         uint32_t gb_addr_cfg;
         uint32_t unk8[21];
         uint32_t num_shader_visible_vgprs;
         uint32_t wave_front_size;
         uint32_t num_shader_engines;
         uint32_t num_active_rb_per_se;
         uint32_t num_shader_arrays_per_engine;
         uint32_t unk9[2];
         uint32_t num_tcc_blocks;
         uint32_t max_gs_waves_per_vgt;
         uint32_t unk10;
         uint32_t gs_vgt_table_depth;
         uint32_t gs_prim_buffer_depth;
         uint32_t unk11[23];
         uint32_t num_cu_per_wgp;
         uint32_t unk12[7];
         uint16_t wgp_bitmap[6][2];
         uint16_t wgp_ao_bitmap[6][2];
         uint32_t unk13[10];
         uint32_t num_sqc_per_wgp;
         uint32_t sqc_inst_cache_size;
         uint32_t sqc_data_cache_size;
         uint32_t gl1c_cache_size;
         uint32_t unk14;
         uint32_t gl2c_cache_size;
         uint32_t unk15[4];
         uint32_t mall_size_mb;
         uint32_t unk16[1824];
         uint64_t va_start;
         uint64_t va_end;
         uint32_t unk17[256];
         uint32_t large_page;
         uint32_t unk18[220];
         struct {
            uint32_t start;
            uint32_t size;
         } ib_alignments[8];
         uint32_t unk19[525];
      };

      struct basic_properties props1 = {0};
      struct gpu_properties props2 = {0};

      query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE, &props1, sizeof(props1));
      query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE, &props2, sizeof(props2));

      /* IP Engines */
      hw_ip_gfx.hw_ip_version_major = 11;
      hw_ip_gfx.hw_ip_version_minor = 0;
      hw_ip_gfx.available_rings = 0x1;
      hw_ip_gfx.ib_start_alignment = props2.ib_alignments[0].start;
      hw_ip_gfx.ib_size_alignment = props2.ib_alignments[0].size;

      hw_ip_compute.hw_ip_version_major = 11;
      hw_ip_compute.hw_ip_version_minor = 0;
      hw_ip_compute.available_rings = 0xf;
      hw_ip_compute.ib_start_alignment = props2.ib_alignments[1].start;
      hw_ip_compute.ib_size_alignment = props2.ib_alignments[1].size;

      /* Firmware versions */
      info->me_fw_version = props2.me_fw_version;
      info->me_fw_feature = 29;
      info->mec_fw_version = props2.mec_fw_version;
      info->mec_fw_feature = 29;
      info->pfp_fw_version = props2.pfp_fw_version;
      info->pfp_fw_feature = 29;

      /* Device identification */
      dev.chip_rev = props2.chip_rev;
      dev.external_rev = props2.chip_external_rev;
      dev.family = props2.family_id;

      /* Shader engine info */
      dev.num_shader_engines = props2.num_shader_engines;
      dev.num_shader_arrays_per_engine = props2.num_shader_arrays_per_engine;
      dev.gpu_counter_freq = props2.clock_crystal_freq_hz / 1000;
      dev.wave_front_size = props2.wave_front_size;
      dev.num_shader_visible_vgprs = props2.num_shader_visible_vgprs;
      dev.num_tcc_blocks = props2.num_tcc_blocks;
      dev.num_rb_pipes = props1.num_rb_pipes;
      dev.enabled_rb_pipes_mask = props1.enabled_rb_pipes_mask;
      dev.enabled_rb_pipes_mask_hi = props1.enabled_rb_pipes_mask_hi;

      /* GS info */
      dev.max_gs_waves_per_vgt = props2.max_gs_waves_per_vgt;
      dev.gs_vgt_table_depth = props2.gs_vgt_table_depth;
      dev.gs_prim_buffer_depth = props2.gs_prim_buffer_depth;

      /* Cache sizes */
      dev.num_sqc_per_wgp = props2.num_sqc_per_wgp;
      dev.sqc_inst_cache_size = props2.sqc_inst_cache_size;
      dev.sqc_data_cache_size = props2.sqc_data_cache_size;
      dev.gl1c_cache_size = props2.gl1c_cache_size;
      dev.gl2c_cache_size = props2.gl2c_cache_size;
      dev.mall_size = (uint64_t)props2.mall_size_mb * 1024 * 1024;

      /* Memory */
#define PAGE_SIZE 4096
      dev.pte_fragment_size = (1 << (props2.large_page & 0x3f)) * PAGE_SIZE;
      dev.vram_type = kmd_to_amdgpu_vram_type(props1.vram_type);
      dev.vram_bit_width = props1.vram_bit_width;
      dev.virtual_address_offset = props2.va_start;
      dev.virtual_address_max = props2.va_end;
      mem.vram.total_heap_size = props2.vram_vis_size + props2.vram_inv_size;
      mem.cpu_accessible_vram.total_heap_size = props2.vram_vis_size;

      /* Cu Mask */
      uint32_t w = props2.num_shader_arrays_per_engine;
      for (uint32_t i = 0; i < props2.num_shader_engines; ++i) {
         for (uint32_t j = 0; j < props2.num_shader_arrays_per_engine; ++j) {
            dev.cu_bitmap[i % 4][i / 4 * w + j] = util_widen_mask(props2.wgp_bitmap[i][j], props2.num_cu_per_wgp);
            dev.cu_ao_bitmap[i % 4][i / 4 * w + j] = util_widen_mask(props2.wgp_ao_bitmap[i][j], props2.num_cu_per_wgp);
         }
      }

      info->gb_addr_config = props2.gb_addr_cfg;
      info->num_tile_pipes = 1 << G_0098F8_NUM_PIPES(info->gb_addr_config);
   }

   info->pcie_gen = 4;
   info->pcie_num_lanes = 16;
   info->has_timeline_syncobj = true;
   info->has_vm_always_valid = true;
   info->spi_cu_en = ~0;
   info->address32_hi = RADV_WDDM2_32BIT_HEAP_START >> 32;

   ac_fill_hw_ip_info(info, &dev, AMD_IP_GFX, &hw_ip_gfx);
   ac_fill_hw_ip_info(info, &dev, AMD_IP_COMPUTE, &hw_ip_compute);
   ac_identify_chip(info, &dev);
   ac_fill_memory_info(info, &dev, &mem);
   ac_fill_hw_info(info, &dev);
   if (has_tiling_info) {
      ac_fill_tiling_info(info, &amdinfo);
   }
   ac_fill_feature_info(info, &dev);
   ac_fill_bug_info(info);
   ac_fill_tess_info(info);

   /* 26.2.2 folds the following into the DRM-specific ac_query_gpu_info master.
    * The wddm2 winsys can't use that, so replicate the fold as local statics. */
   ac_fill_compiler_info(info, &dev, false);
   radv_wddm2_fill_default_max_submitted_ibs(info);
   radv_wddm2_fill_attribute_ring_info(info);
   //set_custom_cu_en_mask(info);
   radv_wddm2_fill_raster_config(info);
   radv_wddm2_fill_scratch_info(info);

   info->compiler_info.has_image_bvh_intersect_ray = false;

   /* WDDM2 has no kernel-side PRT workaround info (amdgpu_sw_info_address_prt_wa_control_bit
    * is DRM-only). Our VA allocator never hands out addresses with bit 47 set, so use the
    * top of the 48-bit VA space as the NULL-PRT control bit: the SMEM fixup then becomes a
    * no-op for every address in use, which matches the Linux split without corrupting VAs. */
   if (info->compiler_info.has_smem_with_null_prt_bug)
      info->address_prt_wa_control_bit = 47;

   return STATUS_SUCCESS;
}

static NTSTATUS
radv_wddm2_query_marketing_name(struct radv_wddm2_winsys *ws, char *name, size_t name_len)
{
   D3DKMT_ADAPTERREGISTRYINFO registry = {};
   NTSTATUS status = query_adapter_info(ws, KMTQAITYPE_ADAPTERREGISTRYINFO_RENDER,
                                        &registry, sizeof(registry));
   if (!NT_SUCCESS(status))
      return status;

   char *old_locale = setlocale(LC_CTYPE, "en_US.utf8");
   size_t len = wcstombs(name, registry.AdapterString, name_len);
   setlocale(LC_CTYPE, old_locale);
   if (len >= name_len)
      return STATUS_INVALID_PARAMETER;

   return STATUS_SUCCESS;
}

static uint64_t
radv_wddm2_winsys_query_value(struct radeon_winsys *_ws, enum radeon_value_id value)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   NTSTATUS status;

   switch (value) {
   case RADEON_ALLOCATED_VRAM:
   case RADEON_ALLOCATED_VRAM_VIS:
   case RADEON_ALLOCATED_GTT: {
      simple_mtx_lock(&ws->alloc_mtx);
      uint64_t ret = 0;
      if (value == RADEON_ALLOCATED_VRAM)
         ret = ws->alloc_vram;
      else if (value == RADEON_ALLOCATED_VRAM_VIS)
         ret = ws->alloc_vram_vis;
      else
         ret = ws->alloc_gtt;
      simple_mtx_unlock(&ws->alloc_mtx);
      return ret;
   }

   case RADEON_VRAM_USAGE:
   case RADEON_VRAM_VIS_USAGE:
   case RADEON_GTT_USAGE: {
      D3DKMT_QUERYVIDEOMEMORYINFO mem_info = {
         .hAdapter = ws->adapter_h,
      };
      switch (value) {
      case RADEON_VRAM_USAGE:
      case RADEON_VRAM_VIS_USAGE:
         mem_info.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
         break;
      case RADEON_GTT_USAGE:
         mem_info.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL;
         break;
      default:
         UNREACHABLE("Invalid value");
      }

      status = WDDM2_DISPATCH(QueryVideoMemoryInfo(&mem_info));
      if (!NT_SUCCESS(status))
         return 0;

      switch (value) {
      case RADEON_VRAM_USAGE:
      case RADEON_VRAM_VIS_USAGE:
      case RADEON_GTT_USAGE:
         return mem_info.CurrentUsage;
      default:
         UNREACHABLE("Invalid value");
      }
   }

   default:
      UNREACHABLE("Unimplemented query");
   }
}

static void
radv_wddm2_winsys_destroy(struct radeon_winsys *_ws)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   ASSERTED NTSTATUS status;
   bool destroy = false;

   simple_mtx_lock(&winsys_creation_mutex);
   if (!--ws->refcount) {
      _mesa_hash_table_remove_key(winsyses, (void *) winsys_luid_key(&ws->adapter_luid));

      /* Clean the hashtable up if empty, though there is no
       * empty function. */
      if (_mesa_hash_table_num_entries(winsyses) == 0) {
         _mesa_hash_table_destroy(winsyses, NULL);
         winsyses = NULL;
      }

      destroy = true;
   }
   simple_mtx_unlock(&winsys_creation_mutex);
   if (!destroy)
      return;

   /* Stop GPU-alive watchdog thread before destroying device */
   ws->watchdog_stop = true;
   WaitForSingleObject(ws->watchdog_thread, 1000);
   CloseHandle(ws->watchdog_thread);

   radv_wddm2_bo_destroy_deferred_all(ws);

   radv_winsys_bo_list_destroy(&ws->global_bo_list);
   radv_winsys_bo_log_destroy(&ws->bo_log);

   radv_wddm2_wsi_finish(ws);

   D3DDDI_DESTROYPAGINGQUEUE destroy_paging_queue = {
      .hPagingQueue = ws->paging_queue_h,
   };
   status = WDDM2_DISPATCH(DestroyPagingQueue(&destroy_paging_queue));
   assert(NT_SUCCESS(status));

   D3DKMT_DESTROYDEVICE destroy_device = {
      .hDevice = ws->device_h,
   };
   status = WDDM2_DISPATCH(DestroyDevice(&destroy_device));
   assert(NT_SUCCESS(status));

   D3DKMT_CLOSEADAPTER close_adapter = {
      .hAdapter = ws->adapter_h,
   };
   status = WDDM2_DISPATCH(CloseAdapter(&close_adapter));
   assert(NT_SUCCESS(status));

   FREE(ws);
}

static uint32_t
radv_wddm2_winsys_get_wddm2_handle(struct radeon_winsys *_ws)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   return ws->device_h;
}

const struct vk_sync_type *const *
radv_wddm2_winsys_get_sync_types(struct radeon_winsys *rws)
{
   return radv_wddm2_winsys(rws)->sync_types;
}

static struct util_sync_provider *
radv_wddm2_winsys_get_sync_provider(struct radeon_winsys *rws)
{
   return NULL;
}

static bool
radv_wddm2_winsys_read_registers(struct radeon_winsys *rws, unsigned reg_offset, unsigned num_registers,
                                 uint32_t *out)
{
   return false;
}

static int
radv_wddm2_winsys_get_fd(struct radeon_winsys *rws)
{
   return -1;
}

static int
radv_wddm2_winsys_reserve_vmid(struct radeon_winsys *rws)
{
   return 0;
}

static void
radv_wddm2_winsys_unreserve_vmid(struct radeon_winsys *rws)
{
}

static VkResult
radv_wddm2_winsys_copy_sync_payloads(struct vk_device *device, uint32_t wait_count,
                                     const struct vk_sync_wait *waits, uint32_t signal_count,
                                     const struct vk_sync_signal *signals)
{
   return VK_SUCCESS;
}

static bool
radv_wddm2_winsys_query_gpuvm_fault(struct radeon_winsys *rws, struct radv_winsys_gpuvm_fault_info *fault_info)
{
   struct radv_wddm2_winsys *ws = (struct radv_wddm2_winsys *)rws;
   NTSTATUS status;

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = ws->device_h,
      .StateType = D3DKMT_DEVICESTATE_PAGE_FAULT,
   };
   status = WDDM2_DISPATCH(GetDeviceState(&get_state));
   if (unlikely(!NT_SUCCESS(status)))
      return false;

   D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

   if (ws->debug_all_bos)
      fprintf(stderr, "faulted VA: 0x%" PRIx64 ", error: 0x%x (vendor specific: %i)\n",
              fault.FaultedVirtualAddress, fault.FaultErrorCode.GeneralErrorCode, fault.FaultErrorCode.DeviceSpecificCode);
   if (!fault.FaultedVirtualAddress)
      return false;

   fault_info->addr = fault.FaultedVirtualAddress;
   fault_info->status = fault.FaultErrorCode.GeneralErrorCode;
   fault_info->vmhub = 0;

   return true;
}

VkResult
radv_wddm2_winsys_query_info(const struct vk_dx_adapter_info *adapter_info,
                             uint64_t debug_flags, struct radeon_winsys_info *info)
{
   VkResult result = VK_SUCCESS;
   NTSTATUS status;

   memset(info, 0, sizeof(*info));

   struct radv_wddm2_winsys tmp_ws;
   memset(&tmp_ws, 0, sizeof(tmp_ws));
   tmp_ws.debug_all_bos = !!(debug_flags & RADV_DEBUG_ALL_BOS);
   tmp_ws.adapter_luid = adapter_info->adapter_luid;

   if (getenv("RADV_WDDM2_TRACE"))
      fprintf(stderr, "[wddm2probe:winsys] query_info enter (luid %x:%x)\n",
              adapter_info->adapter_luid.LowPart, adapter_info->adapter_luid.HighPart);

   D3DKMT_OPENADAPTERFROMLUID open_adapter = {
      .AdapterLuid = adapter_info->adapter_luid,
   };
   status = WDDM2_DISPATCH(OpenAdapterFromLuid(&open_adapter));
   if (!NT_SUCCESS(status))
      return VK_ERROR_INITIALIZATION_FAILED;
   if (getenv("RADV_WDDM2_TRACE"))
      fprintf(stderr, "[wddm2probe:winsys] OpenAdapterFromLuid ok\n");

   tmp_ws.adapter_h = open_adapter.hAdapter;

   status = radv_wddm2_fill_gpu_info(&tmp_ws, adapter_info);
   if (!NT_SUCCESS(status)) {
      if (getenv("RADV_WDDM2_TRACE"))
         fprintf(stderr, "[wddm2probe:winsys] fill_gpu_info failed (%x)\n", (unsigned)status);
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto close_adapter;
   }
   if (getenv("RADV_WDDM2_TRACE"))
      fprintf(stderr, "[wddm2probe:winsys] fill_gpu_info ok\n");

   status = radv_wddm2_query_marketing_name(&tmp_ws, tmp_ws.gpu_info.marketing_name,
                                             sizeof(tmp_ws.gpu_info.marketing_name));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto close_adapter;
   }
   if (getenv("RADV_WDDM2_TRACE"))
      fprintf(stderr, "[wddm2probe:winsys] query_marketing_name ok ('%s')\n", tmp_ws.gpu_info.marketing_name);

   info->base = tmp_ws.gpu_info;

close_adapter:
   {
      D3DKMT_CLOSEADAPTER close_adapter = {
         .hAdapter = tmp_ws.adapter_h,
      };
      WDDM2_DISPATCH(CloseAdapter(&close_adapter));
   }
   return result;
}

VkResult
radv_wddm2_winsys_create(const struct vk_dx_adapter_info *adapter_info,
                          const struct radeon_info *info, uint64_t debug_flags,
                          uint64_t perftest_flags, struct radeon_winsys **winsys)
{
   VkResult result = VK_SUCCESS;
   struct radv_wddm2_winsys *ws = NULL;
   NTSTATUS status;

   fprintf(stderr, "[wddm2:winsys] create enter (luid %x:%x)\n",
           (unsigned)adapter_info->adapter_luid.LowPart,
           (unsigned)adapter_info->adapter_luid.HighPart);

   /* We have to keep this lock till insertion. */
   simple_mtx_lock(&winsys_creation_mutex);
   if (!winsyses)
      winsyses = _mesa_pointer_hash_table_create(NULL);
   if (!winsyses) {
      fprintf(stderr, "radv/amdgpu: failed to alloc winsys hash table.\n");
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   struct hash_entry *entry = _mesa_hash_table_search(winsyses,
                                                       (void *) winsys_luid_key(&adapter_info->adapter_luid));
   if (entry) {
      ws = (struct radv_wddm2_winsys *)entry->data;
      ++ws->refcount;
      fprintf(stderr, "[wddm2:winsys] create: found existing winsys, refcount=%u\n", ws->refcount);
   }
   
   if (ws) {
      simple_mtx_unlock(&winsys_creation_mutex);
      *winsys = &ws->base;
      return VK_SUCCESS;
   }

   fprintf(stderr, "[wddm2:winsys] create: allocating new winsys\n");
   ws = calloc(1, sizeof(struct radv_wddm2_winsys));
   if (!ws) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   ws->refcount = 1;
   ws->adapter_luid = adapter_info->adapter_luid;
   ws->chain_ib = !(debug_flags & RADV_DEBUG_NO_IB_CHAINING);
   ws->debug_all_bos = !!(debug_flags & RADV_DEBUG_ALL_BOS);
   ws->debug_log_bos = debug_flags & RADV_DEBUG_HANG;
   ws->dump_ibs = !!(debug_flags & RADV_DEBUG_DUMP_IBS);
   ws->dbg = !!getenv("RADV_WDDM2_DBG");
   radv_winsys_bo_list_init(&ws->global_bo_list);
   radv_winsys_bo_log_init(&ws->bo_log, debug_flags);
    simple_mtx_init(&ws->deferred_mtx, mtx_plain);
    list_inithead(&ws->deferred_list);
    simple_mtx_init(&ws->gpu_mtx, mtx_plain);
   simple_mtx_init(&ws->d3d_mtx, mtx_plain);
   simple_mtx_init(&ws->alloc_mtx, mtx_plain);
   ws->alloc_vram = 0;
   ws->alloc_vram_vis = 0;
   ws->alloc_gtt = 0;
   simple_mtx_init(&ws->va_mtx, mtx_plain);
   ws->va_live = 0;
   ws->va_mapped_total = 0;
   ws->va_freed_total = 0;

   D3DKMT_OPENADAPTERFROMLUID open_adapter = {
      .AdapterLuid = ws->adapter_luid,
   };
   fprintf(stderr, "[wddm2:winsys] create: OpenAdapterFromLuid...\n");
   status = WDDM2_DISPATCH(OpenAdapterFromLuid(&open_adapter));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Can't open adapter with luid %X%X\n", adapter_info->adapter_luid.LowPart, adapter_info->adapter_luid.HighPart);
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_ptr_alloc;
   }
   fprintf(stderr, "[wddm2:winsys] create: OpenAdapterFromLuid ok, hAdapter=%i\n", open_adapter.hAdapter);

   ws->adapter_h = open_adapter.hAdapter;

   ws->gpu_info = *info;

   D3DKMT_CREATEDEVICE create_device = {
      .hAdapter = ws->adapter_h,
   };
   fprintf(stderr, "[wddm2:winsys] create: CreateDevice...\n");
   status = WDDM2_DISPATCH(CreateDevice(&create_device));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Couldn't create device for adapter %i\n", ws->adapter_h);
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_open_adapter;
   }
   fprintf(stderr, "[wddm2:winsys] create: CreateDevice ok, hDevice=%i\n", create_device.hDevice);

   ws->device_h = create_device.hDevice;

   D3DKMT_CREATEPAGINGQUEUE create_paging_queue = {
      .hDevice = ws->device_h,
   };
   fprintf(stderr, "[wddm2:winsys] create: CreatePagingQueue...\n");
   status = WDDM2_DISPATCH(CreatePagingQueue(&create_paging_queue));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Couldn't create paging queue\n");
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto error_create_device;
   }
   fprintf(stderr, "[wddm2:winsys] create: CreatePagingQueue ok\n");

   ws->paging_queue_h = create_paging_queue.hPagingQueue;
   ws->paging_fence_h = create_paging_queue.hSyncObject;

   ws->base.destroy = radv_wddm2_winsys_destroy;
   ws->base.query_value = radv_wddm2_winsys_query_value;
   ws->base.get_wddm2_handle = radv_wddm2_winsys_get_wddm2_handle;
   ws->base.get_sync_provider = radv_wddm2_winsys_get_sync_provider;
   ws->base.query_gpuvm_fault = radv_wddm2_winsys_query_gpuvm_fault;
   ws->base.read_registers = radv_wddm2_winsys_read_registers;
   ws->base.get_fd = radv_wddm2_winsys_get_fd;
   ws->base.reserve_vmid = radv_wddm2_winsys_reserve_vmid;
   ws->base.unreserve_vmid = radv_wddm2_winsys_unreserve_vmid;
   ws->base.copy_sync_payloads = radv_wddm2_winsys_copy_sync_payloads;
   ws->base.gpu_info = &ws->gpu_info;
   radv_wddm2_bo_init_functions(ws);
   radv_wddm2_cs_init_functions(ws);

   ws->sync_binary_type = vk_sync_binary_get_type(&vk_wddm2_monitored_fence_type);
   ws->sync_types[0] = &vk_wddm2_monitored_fence_type;
   ws->sync_types[1] = &ws->sync_binary_type.sync;
   ws->sync_types[2] = NULL;

   /* Start GPU-alive watchdog thread */
   ws->watchdog_stop = false;
   ws->watchdog_thread = CreateThread(NULL, 0, radv_wddm2_watchdog_thread, ws, 0, NULL);

   _mesa_hash_table_insert(winsyses, (void *) winsys_luid_key(&ws->adapter_luid), ws);
   simple_mtx_unlock(&winsys_creation_mutex);

   *winsys = &ws->base;

   return result;

error_create_device:
   {
      D3DKMT_DESTROYDEVICE destroy_device = {
         .hDevice = ws->device_h,
      };
      status = WDDM2_DISPATCH(DestroyDevice(&destroy_device));
      assert(NT_SUCCESS(status));
   }

error_open_adapter:
   {
      D3DKMT_CLOSEADAPTER close_adapter = {
         .hAdapter = ws->adapter_h,
      };
      status = WDDM2_DISPATCH(CloseAdapter(&close_adapter));
      assert(NT_SUCCESS(status));
   }

error_ptr_alloc:
   FREE(ws);

fail:
   simple_mtx_unlock(&winsys_creation_mutex);
   return result;
}
