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

#include "radv_wddm2_cs.h"
#include "radv_wddm2_bo.h"
#include "radv_winsys_cs.h"
#include "vk_async_event.h"
#include "vk_util.h"
#include "vk_wddm2_monitored_fence.h"
#include "util/macros.h"
#include "util/set.h"
#include "util/u_memory.h"
#include "ac_debug.h"
#include "radv_cs.h"

#include "amd/common/sid.h"

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

#include <assert.h>
#include <d3dkmthk.h>

struct PACKED create_context_private_data {
   uint32_t header_size;
   uint32_t reserved[15];
};
static_assert(sizeof(struct create_context_private_data) == 0x40, "This struct has no holes");

struct PACKED create_queue_private_data {
   uint32_t header_size;
   uint32_t reserved[3];
   uint32_t flags;
   uint32_t queue_id;
   uint32_t parent_queue_id;
   uint32_t reserved2[9];
};
static_assert(sizeof(struct create_queue_private_data) == 0x40, "This struct has no holes");

struct submit_pdd_writer {
   uint32_t num_entries;
   uint8_t *buffer;
   unsigned buffer_size;
   unsigned offset;
   uint8_t inline_data[256];
};

struct submit_pdd_header {
   uint32_t version;
   uint32_t num_entries;
};

struct submit_pdd_entry {
   uint32_t type;
   uint32_t size;
};

struct submit_pdd_gfx_ib {
   struct submit_pdd_entry base;
   uint32_t len;
   uint32_t flags;
   uint32_t _dw_2;
   uint32_t ip_type;
   uint32_t addr_lo;
   uint32_t addr_hi;
   uint32_t _dw_5;
   uint32_t _dw_7;
   uint32_t _dw_8;
   uint32_t _dw_9;
   uint32_t _dw_a;
   uint32_t _dw_b;
   uint32_t _dw_c;
   uint32_t _dw_d;
};
static_assert(sizeof(struct submit_pdd_gfx_ib) == 64, "This struct has no holes");

struct submit_pdd_gang_queue {
   struct submit_pdd_entry base;
   uint32_t always_1;
   uint32_t queue_id;
   uint32_t num_ibs;
   uint32_t _dw3;
};

struct submit_pdd_gang_ib {
   uint32_t len;
   uint32_t flags;
   uint32_t addr_lo;
   uint32_t addr_hi;
};

enum submit_pdd_entry_type {
   SUBMIT_PDD_ENTRY_GFX_IB = 0,
   SUBMIT_PDD_ENTRY_GANG_QUEUE = 6,
};

static void
submit_pdd_writer_init(struct submit_pdd_writer *writer)
{
   writer->num_entries = 0;
   writer->buffer = writer->inline_data;
   writer->buffer_size = sizeof(writer->inline_data);
   writer->offset = sizeof(struct submit_pdd_header);
}

static void
submit_pdd_writer_finalize(struct submit_pdd_writer *writer)
{
   struct submit_pdd_header *header = (struct submit_pdd_header *)writer->buffer;
   header->version = 0x9;
   header->num_entries = writer->num_entries;
}

static void
submit_pdd_writer_destroy(struct submit_pdd_writer *writer)
{
   if (writer->buffer != writer->inline_data)
      free(writer->buffer);
}

static void *
submit_pdd_writer_reserve(struct submit_pdd_writer *writer, unsigned size)
{
   if (writer->offset + size > writer->buffer_size) {
      uint8_t *old_buffer = writer->buffer;
      writer->buffer_size = MAX2(writer->buffer_size * 2, writer->offset + size);
      if (old_buffer == writer->inline_data) {
         writer->buffer = malloc(writer->buffer_size);
         if (!writer->buffer)
            return NULL;
         memcpy(writer->buffer, old_buffer, writer->offset);
      } else {
         uint8_t *new_buffer = realloc(old_buffer, writer->buffer_size);
         if (!new_buffer)
            return NULL;
         writer->buffer = new_buffer;
      }
   }
   void *ptr = writer->buffer + writer->offset;
   memset(ptr, 0, size);
   writer->offset += size;
   return ptr;
}

static void
radv_wddm2_queue_destroy(struct radv_wddm2_queue *queue)
{
   if (queue->vm_fence.handle) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy_fence = {
         .hSyncObject = queue->vm_fence.handle,
      };
      WDDM2_DISPATCH(DestroySynchronizationObject(&destroy_fence));
      queue->vm_fence.handle = 0;
   }
   if (queue->handle) {
      D3DKMT_DESTROYHWQUEUE queue_destroy = {
         .hHwQueue = queue->handle,
      };
      WDDM2_DISPATCH(DestroyHwQueue(&queue_destroy));
      queue->handle = 0;
   }
   if (queue->hw_context_h) {
      D3DKMT_DESTROYHWCONTEXT ctx_destroy = {
         .hHwContext = queue->hw_context_h,
      };
      WDDM2_DISPATCH(DestroyHwContext(&ctx_destroy));
      queue->hw_context_h = 0;
   }
   if (queue->context_h) {
      D3DKMT_DESTROYCONTEXT context_destroy = {
         .hContext = queue->context_h,
      };
      WDDM2_DISPATCH(DestroyContext(&context_destroy));
      queue->context_h = 0;
   }
}

static VkResult
radv_wddm2_queue_init(struct radv_wddm2_winsys *ws, enum amd_ip_type hw_ip,
                      enum radeon_ctx_priority priority, struct radv_wddm2_queue *parent,
                      struct radv_wddm2_queue *queue)
{
   NTSTATUS status;
   uint32_t node;

   queue->hw_ip = hw_ip;

   switch (hw_ip) {
   case AMD_IP_GFX:
      /* The 3-D node, discovered from the KMD at winsys creation. */
      node = debug_get_num_option("RADV_DXGI_3D_NODE", ws->gfx_node);
      break;
   case AMD_IP_COMPUTE:
      /* Prefer the dedicated compute/ACE node when the adapter exposes one
       * (multi-node parts such as Vega/Navi); otherwise the 3-D node hosts the
       * compute/ACE engines (single-node parts such as Polaris).  The env
       * override pins a specific node for debugging. */
      node = debug_get_num_option("RADV_DXGI_COMPUTE_NODE",
                                  ws->has_dedicated_compute_node ? ws->compute_node
                                                                  : ws->gfx_node);
      break;
   default:
      /* Not supported */
      return VK_SUCCESS;
   }

   struct create_context_private_data create_context_data = {
      .header_size = 0x40,
   };

   D3DKMT_CREATECONTEXTVIRTUAL create_context = {
      .hDevice = ws->device_h,
      .NodeOrdinal = node,
      /* EngineAffinity is a bitmask selecting engine instance(s) within the
       * node.  We use engine 0, which is the only engine exposed on single-node
       * parts (Polaris) and the first engine on multi-engine nodes.
       *
       * TODO: The standard WDDM queries (NodeMetadata, QueryStatistics) do not
       * expose how many physical engine instances a node contains, so the mask
       * cannot be derived portably here.  Multi-engine nodes (RDNA-class parts
       * with several engines per node) are not selectable beyond engine 0 until
       * a KMD-specific path that reports per-node engine counts is implemented.
       */
      .EngineAffinity = 1,
      .Flags = {
         .DisableGpuTimeout = true,
      },
      .pPrivateDriverData = &create_context_data,
      .PrivateDriverDataSize = sizeof(create_context_data),
      .ClientHint = D3DKMT_CLIENTHINT_VULKAN,
   };

   status = WDDM2_DISPATCH(CreateContextVirtual(&create_context));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Create context failed 0x%X for IP %i and device 0x%x\n", status, hw_ip, ws->device_h);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   queue->context_h = create_context.hContext;

   if (hw_ip == AMD_IP_GFX || hw_ip == AMD_IP_COMPUTE) {
      /* Fast submission path: create a hardware context + hardware queue so we
       * can use D3DKMT_SUBMITCOMMANDTOHWQUEUE instead of the legacy
       * broadcast-context D3DKMT_SUBMITCOMMAND, which is ~50-260us slower per
       * submit on this backend. If either step fails we keep queue->handle == 0
       * and fall back to the legacy path. */
      D3DKMT_CREATEHWCONTEXT create_hw_context = {
         .hDevice = ws->device_h,
         .NodeOrdinal = node,
         /* EngineAffinity = engine 0 (see TODO at CreateContextVirtual site;
          * per-node engine count is not exposed portably by WDDM). */
         .EngineAffinity = 1,
         .PrivateDriverDataSize = sizeof(create_context_data),
         .pPrivateDriverData = &create_context_data,
      };
      status = WDDM2_DISPATCH(CreateHwContext(&create_hw_context));
      if (NT_SUCCESS(status)) {
         queue->hw_context_h = create_hw_context.hHwContext;

         D3DKMT_CREATEHWQUEUE create_hw_queue = {
            .hHwContext = queue->hw_context_h,
            .PrivateDriverDataSize = 0,
            .pPrivateDriverData = NULL,
         };
         status = WDDM2_DISPATCH(CreateHwQueue(&create_hw_queue));
         if (NT_SUCCESS(status)) {
            queue->handle = create_hw_queue.hHwQueue;
            queue->hq_progress_fence_cpu =
               (uint64_t *)create_hw_queue.HwQueueProgressFenceCPUVirtualAddress;
         } else {
            WDDM2_DISPATCH(DestroyHwContext(&(D3DKMT_DESTROYHWCONTEXT){ .hHwContext = queue->hw_context_h }));
            queue->hw_context_h = 0;
         }
      }

      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create_sync = {
         .hDevice = ws->device_h,
         .Info = {
            .Type = D3DDDI_MONITORED_FENCE,
            .MonitoredFence = {
               /* EngineAffinity = engine 0 (same node as the context above). */
               .EngineAffinity = 1,
            },
         }
      };
      status = WDDM2_DISPATCH(CreateSynchronizationObject2(&create_sync));
      if (unlikely(!NT_SUCCESS(status))) {
         fprintf(stderr, "CreateSynchronizationObject2 failed with NTSTATUS 0x%x\n", status);
         goto failed;
      }
      queue->vm_fence.handle = create_sync.hSyncObject;
   }

   return VK_SUCCESS;

failed:
   radv_wddm2_queue_destroy(queue);
   return VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult
radv_wddm2_ctx_is_priority_permitted(struct radeon_winsys *_ws, enum radeon_ctx_priority priority);

static VkResult
radv_wddm2_ctx_create(struct radeon_winsys *_ws, enum radeon_ctx_priority priority,
                      struct radeon_winsys_ctx **rctx)
{
   VkResult result;
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (radv_wddm2_ctx_is_priority_permitted(_ws, priority) != VK_SUCCESS)
      return VK_ERROR_NOT_PERMITTED;

   struct radv_wddm2_ctx *ctx = CALLOC_STRUCT(radv_wddm2_ctx);
   if (!ctx)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   ctx->ws = ws;

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      result = radv_wddm2_queue_init(ws, ip, priority, NULL, &ctx->per_ip[ip].queue);
      if (result != VK_SUCCESS)
         goto fail_contexts;
   }

   if (!ws->default_ctx)
      ws->default_ctx = ctx;

   *rctx = (struct radeon_winsys_ctx *)ctx;
   return VK_SUCCESS;

fail_contexts:
   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_destroy(&ctx->per_ip[ip].queue);

   FREE(ctx);
   return result;
}

static void
radv_wddm2_ctx_destroy(struct radeon_winsys_ctx *rwctx)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_destroy(&ctx->per_ip[ip].queue);
   radv_wddm2_queue_destroy(&ctx->ace_queue);

   FREE(ctx);
}

static VkResult
radv_wddm2_ctx_is_priority_permitted(struct radeon_winsys *_ws, enum radeon_ctx_priority priority)
{
   if (priority >= RADEON_CTX_PRIORITY_LOW && priority <= RADEON_CTX_PRIORITY_HIGH)
      return VK_SUCCESS;
   return VK_ERROR_NOT_PERMITTED;
}

static int
radv_wddm2_ctx_set_pstate(struct radeon_winsys_ctx *rwctx, uint32_t pstate)
{
   return 0;
}

bool
vk_wddm2_fence_wait(uint32_t device_h, struct vk_wddm2_fence *fence)
{
   VkResult result;
   NTSTATUS status;
   HANDLE async_event = 0;

   for (;;) {
      /* Quick poll the fence ourselves.  We may not have to call into the
       * kernel at all.
       */
      if (p_atomic_read(fence->value_map) >= fence->wait_value)
         return true;

      result = vk_async_event_create(&async_event);
      if (unlikely(result != VK_SUCCESS))
         return false;

      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
         .hDevice = device_h,
         .ObjectCount = 1,
         .ObjectHandleArray = &fence->handle,
         .FenceValueArray = &fence->wait_value,
         .hAsyncEvent = async_event,
      };
      status = WDDM2_DISPATCH(WaitForSynchronizationObjectFromCpu(&wait));

      if (unlikely(!NT_SUCCESS(status))) {
         vk_async_event_close(async_event);
         fprintf(stderr, "fence wait failed: 0x%X\n", status);
         return false;
      }

      result = vk_async_event_wait(async_event, 10000000000ull);
      vk_async_event_close(async_event);

      D3DKMT_GETDEVICESTATE get_state = {
         .hDevice = device_h,
         .StateType = D3DKMT_DEVICESTATE_EXECUTION,
      };
      status = WDDM2_DISPATCH(GetDeviceState(&get_state));

      if (NT_SUCCESS(status) && get_state.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT) {
         get_state.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
         status = WDDM2_DISPATCH(GetDeviceState(&get_state));
         D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

         fprintf(stderr, "faulted VA: 0x%" PRIx64 ", error: 0x%x (vendor specific: %i), flags: %i, stage: %i\n",
                fault.FaultedVirtualAddress, fault.FaultErrorCode.GeneralErrorCode,
                fault.FaultErrorCode.DeviceSpecificCode, fault.PageFaultFlags, fault.FaultedPipelineStage);
         return false;
      }

      /* The shared value_map is the source of truth: the kernel may signal the
       * async event marginally before (or after) the fence value is committed
       * to the monitored-fence CPU address.  Only report completion once the
       * actual value reflects it, re-arming the wait when the event raced.
       */
      if (p_atomic_read(fence->value_map) >= fence->wait_value)
         return true;

      if (result != VK_SUCCESS) {
         fprintf(stderr, "async wait event: 0x%x\n", result);
         return false;
      }
   }
}

static bool
radv_wddm2_ctx_wait_idle(struct radeon_winsys_ctx *rwctx, enum amd_ip_type ip_type, int ring_index)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);
   bool ret = true;

   if (ctx->per_ip[ip_type].last_submission.handle)
      ret = vk_wddm2_fence_wait(ctx->ws->device_h, &ctx->per_ip[ip_type].last_submission);

   return ret;
}

struct radv_wddm2_cs {
   struct radv_winsys_cs base;
   struct set *buffers;
};

static inline struct radv_wddm2_cs *
radv_wddm2_cs(struct ac_cmdbuf *base)
{
   return (struct radv_wddm2_cs *)base;
}

static enum radeon_bo_domain
radv_wddm2_cs_domain(const struct radeon_winsys *_ws)
{
   return RADEON_DOMAIN_GTT;
}

static struct ac_cmdbuf *
radv_wddm2_cs_create(struct radeon_winsys *rws, enum amd_ip_type ip_type, bool is_secondary)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(rws);
   struct radv_wddm2_cs *cs;

   cs = calloc(1, sizeof(struct radv_wddm2_cs));
   if (!cs)
      return NULL;

   cs->buffers = _mesa_pointer_set_create(NULL);

   VkResult result = radv_winsys_cs_init(&cs->base, rws, ip_type, is_secondary, ws->chain_ib);
   if (result != VK_SUCCESS) {
      _mesa_set_destroy(cs->buffers, NULL);
      free(cs);
      return NULL;
   }

   _mesa_set_add(cs->buffers, radv_wddm2_bo(cs->base.ib_buffer));

   return &cs->base.base;
}


static uint32_t
radv_wddm2_cs_translate_ip_type(enum amd_ip_type ip_type)
{
   return (ip_type == AMD_IP_SDMA) ? 0 : 1;
}

static void
radv_wddm2_cs_destroy(struct ac_cmdbuf *_cs)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);

   radv_winsys_cs_destroy(&cs->base);
   _mesa_set_destroy(cs->buffers, NULL);

   FREE(cs);
}

static void
radv_wddm2_cs_reset(struct ac_cmdbuf *_cs)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);

   radv_winsys_cs_reset(&cs->base);
   _mesa_set_clear(cs->buffers, NULL);
}

static void
radv_wddm2_cs_add_buffer(struct ac_cmdbuf *_cs, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);
   _mesa_set_add(cs->buffers, radv_wddm2_bo(_bo));
}

static void
radv_wddm2_cs_execute_secondary(struct ac_cmdbuf *_parent, struct ac_cmdbuf *_child,
                                bool allow_ib2)
{
   radv_winsys_cs_execute_secondary(radv_winsys_cs(_parent), radv_winsys_cs(_child), allow_ib2);
}

#define WRITE_STRUCT(pdd, type, item) \
   for (struct submit_pdd_##type *item = submit_pdd_writer_reserve(pdd, sizeof(struct submit_pdd_##type)); \
        item != NULL; \
        item = NULL)

#define WRITE_ENTRY(pdd, TYPE, typ, entry) \
   for (struct submit_pdd_##typ *entry = submit_pdd_writer_reserve(pdd, sizeof(struct submit_pdd_##typ)); \
        entry != NULL && \
        (((struct submit_pdd_entry *) entry)->size = sizeof(struct submit_pdd_##typ)); \
        ((struct submit_pdd_entry *) entry)->type = SUBMIT_PDD_ENTRY_##TYPE, \
        (pdd)->num_entries++, \
         entry = NULL)

static void
radv_wddm2_submit_add_cs(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                         struct radv_winsys_cs *cs, enum radv_cs_dump_type type, bool is_gang,
                         struct radv_winsys_ib *first)
{
   const unsigned num_ib_buffers = cs->chain_ib ? 1 : cs->num_ib_buffers;
   uint32_t flags = 0;

   if (type == RADV_CS_DUMP_TYPE_PREAMBLE_IBS)
      flags = 0xc;
   else if (type == RADV_CS_DUMP_TYPE_POSTAMBLE_IBS)
      flags = 0x104;

   if (getenv("RADV_WDDM2_DUMP_MAIN") && type == RADV_CS_DUMP_TYPE_MAIN_IBS)
      ctx->ws->base.cs_dump(&cs->base, stderr, NULL, 0, type);

   if (ctx->ws->dump_ibs)
      ctx->ws->base.cs_dump(&cs->base, stderr, NULL, 0, type);

for (unsigned i = 0; i < num_ib_buffers; i++) {
      struct radv_winsys_ib ib = cs->ib_buffers[i];

      if (ctx->ws->dump_ibs)
         fprintf(stderr, "RADV_WDDM2_DBG   ib[%u] va=0x%llx cdw=%u hw_ip=%u\n", i,
                 (unsigned long long)ib.va, ib.cdw, cs->hw_ip);

      if (first->va == 0)
         *first = ib;

      if (is_gang) {
         WRITE_STRUCT(pdd, gang_ib, gang_ib) {
            gang_ib->len = ib.cdw * 4;
            gang_ib->flags = flags;
            gang_ib->addr_lo = ib.va;
            gang_ib->addr_hi = ib.va >> 32;
         }
      } else {
         WRITE_ENTRY(pdd, GFX_IB, gfx_ib, entry) {
            entry->len = ib.cdw * 4;
            entry->flags = flags;
            entry->ip_type = radv_wddm2_cs_translate_ip_type(cs->hw_ip);
            entry->addr_lo = ib.va;
            entry->addr_hi = ib.va >> 32;
         }
      }
   }
}

static unsigned
radv_wddm2_count_submitted_ibs(const struct radv_winsys_submit_info *submit, unsigned hw_ip)
{
   unsigned num_ibs = 0;

   for (unsigned i = 0; i < submit->initial_preamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->initial_preamble_cs[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   for (unsigned i = 0; i < submit->cs_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->cs_array[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   for (unsigned i = 0; i < submit->postamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->postamble_cs[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   return num_ibs;
}

static void
radv_wddm2_cs_submit_add_ibs(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                             const struct radv_winsys_submit_info *submit, unsigned hw_ip,
                             struct radv_winsys_ib *first_ib)
{
   for (unsigned i = 0; i < submit->initial_preamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->initial_preamble_cs[i]);
      assert(cs->num_ib_buffers == 1);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_PREAMBLE_IBS,
                               submit->is_gang, first_ib);
   }

   for (unsigned i = 0; i < submit->cs_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->cs_array[i]);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_MAIN_IBS,
                               submit->is_gang, first_ib);
   }

   for (unsigned i = 0; i < submit->postamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->postamble_cs[i]);
      assert(cs->num_ib_buffers == 1);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_POSTAMBLE_IBS,
                               submit->is_gang, first_ib);
   }
}

static void
radv_wddm2_submit_add_queue(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                             const struct radv_winsys_submit_info *submit, struct radv_wddm2_queue *queue,
                             struct radv_winsys_ib *first_ib)
{
   WRITE_ENTRY(pdd, GANG_QUEUE, gang_queue, entry) {
      entry->num_ibs = radv_wddm2_count_submitted_ibs(submit, queue->hw_ip);
      entry->base.size += entry->num_ibs * sizeof(struct submit_pdd_gang_ib);
      entry->always_1 = 1;
      entry->queue_id = queue->queue_id;
   }

   radv_wddm2_cs_submit_add_ibs(ctx, pdd, submit, queue->hw_ip, first_ib);
}

static VkResult
radv_wddm2_cs_submit(struct radeon_winsys_ctx *_ctx,
                     const struct radv_winsys_submit_info *submit,
                     uint32_t wait_count, const struct vk_sync_wait *waits,
                     uint32_t signal_count, const struct vk_sync_signal *signals)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(_ctx);
   struct radv_wddm2_queue *queue = &ctx->per_ip[submit->ip_type].queue;
   struct radv_wddm2_queue *ace_queue = &ctx->ace_queue;
   NTSTATUS status;

   if (getenv("RADV_WDDM2_DBG"))
      fprintf(stderr, "RADV_WDDM2_DBG submit ip=%u gang=%d cs=%u dump_ibs=%d hwqueue=%llu ctx=%llu\n",
              submit->ip_type, submit->is_gang ? 1 : 0, submit->cs_count,
              ctx->ws->dump_ibs ? 1 : 0, (unsigned long long)queue->handle,
              (unsigned long long)queue->context_h);

   if (ctx->ws->dump_ibs && !ctx->ws->has_dedicated_compute_node &&
       !submit->is_gang && submit->ip_type == AMD_IP_COMPUTE)
      fprintf(stderr, "ACE-ON-GFX: routing compute submit via GFX queue\n");

   /* Standalone COMPUTE (ACE) submissions to the compute context fault the GPU
    * on SINGLE-NODE parts (Polaris: the only compute context that can be created
    * there ends up on the same node the KMD schedules as node 2, which faults),
    * producing a GPU exception (VK_ERROR_DEVICE_LOST) on the next submit. This
    * is hit by D3D9-via-DXVK workloads that issue compute submissions, but not
    * by native Vulkan present paths (which only use GFX). Route non-gang
    * compute command buffers through the GFX hw context/queue, which is verified
    * working; this serializes the (rare, D3D9) standalone compute work on the
    * GFX engine instead of faulting. Gang submits already submit compute via the
    * AIB queue and are unaffected.
    *
    * On multi-node parts (Vega/Navi, has_dedicated_compute_node) a dedicated
    * compute node is discovered and used, so this workaround is NOT applied. */
   if (!submit->is_gang && submit->ip_type == AMD_IP_COMPUTE &&
       !ctx->ws->has_dedicated_compute_node)
      queue = &ctx->per_ip[AMD_IP_GFX].queue;

   assert(queue->context_h != 0 && "Unsupported IP type");

   if (submit->is_gang && ace_queue->context_h == 0) {
      assert(submit->ip_type == AMD_IP_GFX);
      radv_wddm2_queue_init(ctx->ws, AMD_IP_COMPUTE, 0, queue, ace_queue);
      if (ace_queue->context_h == 0)
         return VK_ERROR_DEVICE_LOST;
   }

   if (wait_count > 0) {
      STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
      STACK_ARRAY(uint64_t, values, wait_count);

      for (uint32_t i = 0; i < wait_count; i++) {
         handles[i] = vk_sync_as_wddm2_monitored_fence(waits[i].sync)->handle;
         values[i] = waits[i].wait_value;
      }

      if (queue->handle) {
         D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE wait = {
            .hHwQueue = queue->handle,
            .ObjectCount = wait_count,
            .ObjectHandleArray = handles,
            .FenceValueArray = values,
         };
         status = WDDM2_DISPATCH(SubmitWaitForSyncObjectsToHwQueue(&wait));
      } else {
         D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {
            .hContext = queue->context_h,
            .ObjectCount = wait_count,
            .ObjectHandleArray = handles,
            .MonitoredFenceValueArray = values,
         };
         status = WDDM2_DISPATCH(WaitForSynchronizationObjectFromGpu(&wait));
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      assert(NT_SUCCESS(status));
      if (!NT_SUCCESS(status))
         return VK_ERROR_DEVICE_LOST;
   }

   if (submit->cs_count > 0) {
      struct radv_winsys_ib first_ib = {};
      struct submit_pdd_writer pdd;

      submit_pdd_writer_init(&pdd);
      if (submit->is_gang) {
         radv_wddm2_submit_add_queue(ctx, &pdd, submit, queue, &first_ib);
         radv_wddm2_submit_add_queue(ctx, &pdd, submit, ace_queue, &first_ib);
      } else {
         radv_wddm2_cs_submit_add_ibs(ctx, &pdd, submit, submit->ip_type, &first_ib);
      }
      submit_pdd_writer_finalize(&pdd);

      if (queue->handle) {
         uint64_t submit_id = p_atomic_inc_return(&queue->hq_submit_seq);
         D3DKMT_SUBMITCOMMANDTOHWQUEUE wddm2_submit = {
            .hHwQueue = queue->handle,
            .HwQueueProgressFenceId = submit_id,
            .CommandBuffer = first_ib.va,
            .CommandLength = first_ib.cdw * 4,
            .pPrivateDriverData = pdd.buffer,
            .PrivateDriverDataSize = pdd.offset,
         };
         status = WDDM2_DISPATCH(SubmitCommandToHwQueue(&wddm2_submit));
      } else {
         D3DKMT_SUBMITCOMMAND wddm2_submit = {
            .Commands = first_ib.va,
            .CommandLength = first_ib.cdw * 4,
            .pPrivateDriverData = pdd.buffer,
            .PrivateDriverDataSize = pdd.offset,
            .BroadcastContextCount = 1,
            .BroadcastContext[0] = queue->context_h,
         };
         status = WDDM2_DISPATCH(SubmitCommand(&wddm2_submit));          
      }
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "SubmitCommand: VK_ERROR_DEVICE_LOST\n");
         return VK_ERROR_DEVICE_LOST;
      }
   }

   if (signal_count > 0) {
      STACK_ARRAY(D3DKMT_HANDLE, handles, signal_count);
      STACK_ARRAY(uint64_t, values, signal_count);

      for (uint32_t i = 0; i < signal_count; i++) {
         handles[i] = vk_sync_as_wddm2_monitored_fence(signals[i].sync)->handle;
         values[i] = signals[i].signal_value;
      }

      if (queue->handle) {
         D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE signal = {
            .Flags = {
               .AllowFenceRewind = 1,
            },
            .BroadcastHwQueueCount = 1,
            .BroadcastHwQueueArray = &queue->handle,
            .ObjectCount = signal_count,
            .ObjectHandleArray = handles,
            .FenceValueArray = values,
         };
         status = WDDM2_DISPATCH(SubmitSignalSyncObjectsToHwQueue(&signal));
      } else {
         D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {
            .ObjectCount = signal_count,
            .ObjectHandleArray = handles,
            .BroadcastContextCount = 1,
            .BroadcastContextArray = &queue->context_h,
            .MonitoredFenceValueArray = values,
         };
         status = WDDM2_DISPATCH(SignalSynchronizationObjectFromGpu2(&signal));
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      assert(NT_SUCCESS(status));
      if (!NT_SUCCESS(status))
         return VK_ERROR_DEVICE_LOST;

      struct vk_wddm2_monitored_fence *fence = vk_sync_as_wddm2_monitored_fence(signals[0].sync);
      ctx->per_ip[submit->ip_type].last_submission.handle = fence->handle;
      ctx->per_ip[submit->ip_type].last_submission.wait_value = signals[0].signal_value;
      ctx->per_ip[submit->ip_type].last_submission.value_map = fence->value_map;

      /* Publish the last submission fence at the winsys level so deferred BO
       * destruction can retire once the GPU has passed this point, preventing a
       * chained IB VA from being reused while still referenced. */
      ctx->ws->last_submission[submit->ip_type] = ctx->per_ip[submit->ip_type].last_submission;
      if (!submit->is_gang && submit->ip_type == AMD_IP_COMPUTE) {
         ctx->per_ip[AMD_IP_GFX].last_submission = ctx->per_ip[submit->ip_type].last_submission;
         ctx->ws->last_submission[AMD_IP_GFX] = ctx->per_ip[submit->ip_type].last_submission;
      }
      radv_wddm2_bo_deferred_drain(ctx->ws);
   }

   return VK_SUCCESS;
}

void
radv_wddm2_cs_init_functions(struct radv_wddm2_winsys *ws)
{
   ws->base.ctx_create = radv_wddm2_ctx_create;
   ws->base.ctx_destroy = radv_wddm2_ctx_destroy;
   ws->base.ctx_set_pstate = radv_wddm2_ctx_set_pstate;
   ws->base.ctx_wait_idle = radv_wddm2_ctx_wait_idle;
   ws->base.cs_domain = radv_wddm2_cs_domain;
   ws->base.cs_create = radv_wddm2_cs_create;
   ws->base.cs_finalize = radv_winsys_cs_finalize;
   ws->base.cs_reset = radv_wddm2_cs_reset;
   ws->base.cs_chain = radv_winsys_cs_chain;
   ws->base.cs_unchain = radv_winsys_cs_unchain;
   ws->base.cs_destroy = radv_wddm2_cs_destroy;
   ws->base.cs_grow = radv_winsys_cs_grow;
   ws->base.cs_add_buffer = radv_wddm2_cs_add_buffer;
   ws->base.cs_submit = radv_wddm2_cs_submit;
   ws->base.cs_execute_secondary = radv_wddm2_cs_execute_secondary;
   ws->base.cs_execute_ib = radv_winsys_cs_execute_ib;
   ws->base.cs_chain_dgc_ib = radv_winsys_cs_chain_dgc_ib;
   ws->base.cs_dump = radv_winsys_cs_dump;
   ws->base.cs_annotate = radv_winsys_cs_annotate;
   ws->base.cs_pad = radv_winsys_cs_pad;
}
