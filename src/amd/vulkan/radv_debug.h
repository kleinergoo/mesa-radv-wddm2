/*
 * Copyright © 2017 Google.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_DEBUG_H
#define RADV_DEBUG_H

#include "radv_device.h"
#include "radv_instance.h"
#include "radv_physical_device.h"

/* Please keep docs/envvars.rst up-to-date when you add/remove options. */
#define RADV_DEBUG_NO_FAST_CLEARS         (1ull << 0)
#define RADV_DEBUG_NO_DCC                 (1ull << 1)
#define RADV_DEBUG_NO_CACHE               (1ull << 3)
#define RADV_DEBUG_DUMP_SHADER_STATS      (1ull << 4)
#define RADV_DEBUG_NO_HIZ                 (1ull << 5)
#define RADV_DEBUG_NO_COMPUTE_QUEUE       (1ull << 6)
#define RADV_DEBUG_ALL_BOS                (1ull << 7)
#define RADV_DEBUG_NO_IB_CHAINING         (1ull << 8)
#define RADV_DEBUG_DUMP_SPIRV             (1ull << 9)
#define RADV_DEBUG_ZERO_VRAM              (1ull << 10)
#define RADV_DEBUG_SYNC_SHADERS           (1ull << 11)
#define RADV_DEBUG_DUMP_PREOPT_IR         (1ull << 12)
#define RADV_DEBUG_INFO                   (1ull << 13)
#define RADV_DEBUG_STARTUP                (1ull << 14)
#define RADV_DEBUG_CHECKIR                (1ull << 15)
#define RADV_DEBUG_NOBINNING              (1ull << 16)
#define RADV_DEBUG_NO_NGG                 (1ull << 17)
#define RADV_DEBUG_DUMP_META_SHADERS      (1ull << 18)
#define RADV_DEBUG_LLVM                   (1ull << 19)
#define RADV_DEBUG_FORCE_COMPRESS         (1ull << 20)
#define RADV_DEBUG_HANG                   (1ull << 21)
#define RADV_DEBUG_IMG                    (1ull << 22)
#define RADV_DEBUG_NO_UMR                 (1ull << 23)
#define RADV_DEBUG_NO_DISPLAY_DCC         (1ull << 24)
#define RADV_DEBUG_NO_TC_COMPAT_CMASK     (1ull << 25)
#define RADV_DEBUG_NO_VRS_FLAT_SHADING    (1ull << 26)
#define RADV_DEBUG_NO_ATOC_DITHERING      (1ull << 27)
#define RADV_DEBUG_NO_NGGC                (1ull << 28)
#define RADV_DEBUG_DUMP_PROLOGS           (1ull << 29)
#define RADV_DEBUG_NO_DMA_BLIT            (1ull << 30)
#define RADV_DEBUG_DUMP_EPILOGS           (1ull << 31)
#define RADV_DEBUG_NO_FMASK               (1ull << 32)
#define RADV_DEBUG_SHADOW_REGS            (1ull << 33)
#define RADV_DEBUG_EXTRA_MD               (1ull << 34)
#define RADV_DEBUG_NO_GPL                 (1ull << 35)
#define RADV_DEBUG_NO_RT                  (1ull << 36)
#define RADV_DEBUG_NO_MESH_SHADER         (1ull << 37)
#define RADV_DEBUG_NO_ESO                 (1ull << 38)
#define RADV_DEBUG_PSO_CACHE_STATS        (1ull << 39)
#define RADV_DEBUG_NIR_DEBUG_INFO         (1ull << 40)
#define RADV_DEBUG_DUMP_TRAP_HANDLER      (1ull << 41)
#define RADV_DEBUG_DUMP_VS                (1ull << 42)
#define RADV_DEBUG_DUMP_TCS               (1ull << 43)
#define RADV_DEBUG_DUMP_TES               (1ull << 44)
#define RADV_DEBUG_DUMP_GS                (1ull << 45)
#define RADV_DEBUG_DUMP_PS                (1ull << 46)
#define RADV_DEBUG_DUMP_TASK              (1ull << 47)
#define RADV_DEBUG_DUMP_MESH              (1ull << 48)
#define RADV_DEBUG_DUMP_CS                (1ull << 49)
#define RADV_DEBUG_DUMP_NIR               (1ull << 50)
#define RADV_DEBUG_DUMP_ASM               (1ull << 51)
#define RADV_DEBUG_DUMP_BACKEND_IR        (1ull << 52)
#define RADV_DEBUG_PSO_HISTORY            (1ull << 53)
#define RADV_DEBUG_BVH4                   (1ull << 54)
#define RADV_DEBUG_NO_VIDEO               (1ull << 55)
#define RADV_DEBUG_VALIDATE_VAS           (1ull << 56)
#define RADV_DEBUG_DUMP_BO_HISTORY        (1ull << 57)
#define RADV_DEBUG_DUMP_IBS               (1ull << 58)
#define RADV_DEBUG_VM                     (1ull << 59)
#define RADV_DEBUG_NO_SMEM_MITIGATION     (1ull << 60)
#define RADV_DEBUG_FULL_SYNC              (1ull << 61)
#define RADV_DEBUG_DUMP_SHADERS ( \
   RADV_DEBUG_DUMP_VS | RADV_DEBUG_DUMP_TCS | RADV_DEBUG_DUMP_TES | \
   RADV_DEBUG_DUMP_GS | RADV_DEBUG_DUMP_PS | RADV_DEBUG_DUMP_TASK | \
   RADV_DEBUG_DUMP_MESH | RADV_DEBUG_DUMP_CS | RADV_DEBUG_DUMP_NIR | \
   RADV_DEBUG_DUMP_ASM | RADV_DEBUG_DUMP_BACKEND_IR )

/* emulate_rt, video_decode, transfer_queue, video_encode, hic, sparse and bfloat16 are deprecated,
 * use RADV_EXPERIMENTAL instead.
 */
enum {
   RADV_PERFTEST_LOCAL_BOS = 1u << 0,
   RADV_PERFTEST_DCC_MSAA = 1u << 1,
   RADV_PERFTEST_CS_WAVE_32 = 1u << 2,
   RADV_PERFTEST_PS_WAVE_32 = 1u << 3,
   RADV_PERFTEST_GE_WAVE_32 = 1u << 4,
   RADV_PERFTEST_NO_SAM = 1u << 5,
   RADV_PERFTEST_SAM = 1u << 6,
   RADV_PERFTEST_NGGC = 1u << 7,
   RADV_PERFTEST_EMULATE_RT = 1u << 8,
   RADV_PERFTEST_RT_WAVE_64 = 1u << 9,
   RADV_PERFTEST_VIDEO_DECODE = 1u << 10,
   RADV_PERFTEST_DMA_SHADERS = 1u << 11,
   RADV_PERFTEST_TRANSFER_QUEUE = 1u << 12,
   RADV_PERFTEST_NIR_CACHE = 1u << 13,
   RADV_PERFTEST_VIDEO_ENCODE = 1u << 14,
   RADV_PERFTEST_NO_GTT_SPILL = 1u << 15,
   RADV_PERFTEST_HIC = 1u << 16,
   RADV_PERFTEST_SPARSE = 1u << 17,
   RADV_PERFTEST_RT_CPS = 1u << 18,
   RADV_PERFTEST_BFLOAT16 = 1u << 19,
   RADV_PERFTEST_LOWLATENCYDEC = 1u << 20,
   RADV_PERFTEST_LOWLATENCYENC = 1u << 21,
};

enum {
   RADV_EXPERIMENTAL_EMULATE_RT = 1u << 0,
   RADV_EXPERIMENTAL_VIDEO_DECODE = 1u << 1,
   RADV_EXPERIMENTAL_TRANSFER_QUEUE = 1u << 2,
   RADV_EXPERIMENTAL_VIDEO_ENCODE = 1u << 3,
   RADV_EXPERIMENTAL_HIC = 1u << 4,
   RADV_EXPERIMENTAL_SPARSE = 1u << 5,
   RADV_EXPERIMENTAL_BFLOAT16 = 1u << 6,
   RADV_EXPERIMENTAL_DESCRIPTOR_HEAP = 1u << 7,
};

enum {
   RADV_TRAP_EXCP_MEM_VIOL = 1u << 0,
   RADV_TRAP_EXCP_FLOAT_DIV_BY_ZERO = 1u << 1,
   RADV_TRAP_EXCP_FLOAT_OVERFLOW = 1u << 2,
   RADV_TRAP_EXCP_FLOAT_UNDERFLOW = 1u << 3,
};

bool radv_init_trace(struct radv_device *device);
void radv_finish_trace(struct radv_device *device);

VkResult radv_check_gpu_hangs(struct radv_queue *queue, const struct radv_winsys_submit_info *submit_info);

void radv_dump_enabled_options(const struct radv_device *device, FILE *f);

bool radv_trap_handler_init(struct radv_device *device);
void radv_trap_handler_finish(struct radv_device *device);
void radv_check_trap_handler(struct radv_queue *queue);

bool radv_vm_fault_occurred(struct radv_device *device, struct radv_winsys_gpuvm_fault_info *fault_info);

ALWAYS_INLINE static bool
radv_device_fault_detection_enabled(const struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);

   return instance->debug_flags & RADV_DEBUG_HANG;
}

struct radv_trace_data {
   uint32_t primary_id;
   uint32_t secondary_id;
   uint64_t gfx_ring_pipeline;
   uint64_t comp_ring_pipeline;
   uint64_t vertex_descriptors;
   uint64_t vertex_prolog;
   uint64_t ps_epilog;
   uint64_t descriptor_sets[MAX_SETS];
   VkDispatchIndirectCommand indirect_dispatch;
};

struct radv_address_binding_report {
   uint64_t timestamp; /* CPU timestamp */
   uint64_t va;
   uint64_t size;
   VkDeviceAddressBindingFlagsEXT flags;
   VkDeviceAddressBindingTypeEXT binding_type;
   uint64_t object_handle;
   VkObjectType object_type;
};

struct radv_address_binding_tracker {
   VkDebugUtilsMessengerEXT messenger;
   struct util_dynarray reports;
   simple_mtx_t mtx;
};

#endif /* RADV_DEBUG_H */
