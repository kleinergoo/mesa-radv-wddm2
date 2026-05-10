/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <libsync.h>
#include <pthread.h>
#include <stdlib.h>
#include "drm-uapi/amdgpu_drm.h"

#include "util/detect_os.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include "ac_cmdbuf_cp.h"
#include "ac_debug.h"
#include "ac_linux_drm.h"
#include "radv_amdgpu_bo.h"
#include "radv_amdgpu_cs.h"
#include "radv_amdgpu_winsys.h"
#include "radv_winsys_cs.h"
#include "radv_debug.h"
#include "radv_radeon_winsys.h"
#include "sid.h"
#include "vk_alloc.h"
#include "vk_drm_syncobj.h"
#include "vk_sync.h"
#include "vk_sync_dummy.h"

/* Some BSDs don't define ENODATA (and ENODATA is replaced with different error
 * codes in the kernel).
 */
#if DETECT_OS_OPENBSD
#define ENODATA ENOTSUP
#elif DETECT_OS_FREEBSD || DETECT_OS_DRAGONFLY
#define ENODATA ECONNREFUSED
#endif

/* Maximum allowed total number of submitted IBs. */
#define RADV_MAX_IBS_PER_SUBMIT 192

struct radv_amdgpu_cs_ib_info {
   int64_t flags;
   uint64_t ib_mc_address;
   uint32_t size;
   enum amd_ip_type ip_type;
};

struct radv_amdgpu_cs {
   struct radv_winsys_cs base;

   struct radv_amdgpu_cs_ib_info ib;
   unsigned max_num_buffers;
   unsigned num_buffers;
   struct drm_amdgpu_bo_list_entry *handles;
   int buffer_hash_table[1024];
};

struct radv_winsys_sem_counts {
   uint32_t syncobj_count;
   uint32_t timeline_syncobj_count;
   uint32_t *syncobj;
   uint64_t *points;
};

struct radv_winsys_sem_info {
   bool cs_emit_signal;
   bool cs_emit_wait;
   struct radv_winsys_sem_counts wait;
   struct radv_winsys_sem_counts signal;
};

static void
radeon_emit_unchecked(struct ac_cmdbuf *cs, uint32_t value)
{
   cs->buf[cs->cdw++] = value;
}

static uint32_t radv_amdgpu_ctx_queue_syncobj(struct radv_amdgpu_ctx *ctx, unsigned ip, unsigned ring);

static inline struct radv_amdgpu_cs *
radv_amdgpu_cs(struct ac_cmdbuf *base)
{
   return (struct radv_amdgpu_cs *)base;
}

struct radv_amdgpu_cs_request {
   /** Specify HW IP block type to which to send the IB. */
   unsigned ip_type;

   /** IP instance index if there are several IPs of the same type. */
   unsigned ip_instance;

   /**
    * Specify ring index of the IP. We could have several rings
    * in the same IP. E.g. 0 for SDMA0 and 1 for SDMA1.
    */
   uint32_t ring;

   /**
    * BO list handles used by this request.
    */
   struct drm_amdgpu_bo_list_entry *handles;
   uint32_t num_handles;

   /** Number of IBs to submit in the field ibs. */
   uint32_t number_of_ibs;

   /**
    * IBs to submit. Those IBs will be submitted together as single entity
    */
   struct radv_amdgpu_cs_ib_info *ibs;

   /**
    * The returned sequence number for the command submission
    */
   uint64_t seq_no;
};

static VkResult radv_amdgpu_cs_submit(struct radv_amdgpu_ctx *ctx, struct radv_amdgpu_cs_request *request,
                                      struct radv_winsys_sem_info *sem_info);

static void
radv_amdgpu_request_to_fence(struct radv_amdgpu_ctx *ctx, struct radv_amdgpu_fence *fence,
                             struct radv_amdgpu_cs_request *req)
{
   fence->fence.ip_type = req->ip_type;
   fence->fence.ip_instance = req->ip_instance;
   fence->fence.ring = req->ring;
   fence->fence.fence = req->seq_no;
}

static struct radv_amdgpu_cs_ib_info
radv_winsys_cs_ib_to_info(struct radv_winsys_cs *cs, struct radv_winsys_ib ib)
{
   struct radv_amdgpu_cs_ib_info info = {
      .flags = 0,
      .ip_type = cs->base.hw_ip,
      .ib_mc_address = ib.va,
      .size = ib.cdw,
   };
   return info;
}

static void
radv_amdgpu_cs_destroy(struct ac_cmdbuf *rcs)
{
   struct radv_amdgpu_cs *cs = radv_amdgpu_cs(rcs);

   radv_winsys_cs_destroy(&cs->base);
   free(cs->handles);
   free(cs);
}

static struct ac_cmdbuf *
radv_amdgpu_cs_create(struct radeon_winsys *_ws, enum amd_ip_type ip_type, bool is_secondary)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_cs *cs;

   cs = calloc(1, sizeof(struct radv_amdgpu_cs));
   if (!cs)
      return NULL;

   VkResult result = radv_winsys_cs_init(&cs->base, _ws, ip_type, is_secondary, ws->chain_ib);
   if (result != VK_SUCCESS) {
      free(cs);
      return NULL;
   }

   for (int i = 0; i < ARRAY_SIZE(cs->buffer_hash_table); ++i)
      cs->buffer_hash_table[i] = -1;

   return &cs->base.base;
}

static void
radv_amdgpu_cs_reset(struct ac_cmdbuf *_cs)
{
   struct radv_amdgpu_cs *cs = radv_amdgpu_cs(_cs);

   for (unsigned i = 0; i < cs->num_buffers; ++i) {
      unsigned hash = cs->handles[i].bo_handle & (ARRAY_SIZE(cs->buffer_hash_table) - 1);
      cs->buffer_hash_table[hash] = -1;
   }

   cs->num_buffers = 0;

   radv_winsys_cs_reset(&cs->base);
}

static int
radv_amdgpu_cs_find_buffer(struct radv_amdgpu_cs *cs, uint32_t bo)
{
   unsigned hash = bo & (ARRAY_SIZE(cs->buffer_hash_table) - 1);
   int index = cs->buffer_hash_table[hash];

   if (index == -1)
      return -1;

   if (cs->handles[index].bo_handle == bo)
      return index;

   for (unsigned i = 0; i < cs->num_buffers; ++i) {
      if (cs->handles[i].bo_handle == bo) {
         cs->buffer_hash_table[hash] = i;
         return i;
      }
   }

   return -1;
}

static void
radv_amdgpu_cs_add_buffer_internal(struct radv_amdgpu_cs *cs, uint32_t bo, uint8_t priority)
{
   unsigned hash;
   int index = radv_amdgpu_cs_find_buffer(cs, bo);

   if (index != -1)
      return;

   if (cs->num_buffers == cs->max_num_buffers) {
      unsigned new_count = MAX2(1, cs->max_num_buffers * 2);
      struct drm_amdgpu_bo_list_entry *new_entries =
         realloc(cs->handles, new_count * sizeof(struct drm_amdgpu_bo_list_entry));
      if (new_entries) {
         cs->max_num_buffers = new_count;
         cs->handles = new_entries;
      } else {
         cs->base.status = VK_ERROR_OUT_OF_HOST_MEMORY;
         return;
      }
   }

   cs->handles[cs->num_buffers].bo_handle = bo;
   cs->handles[cs->num_buffers].bo_priority = priority;

   hash = bo & (ARRAY_SIZE(cs->buffer_hash_table) - 1);
   cs->buffer_hash_table[hash] = cs->num_buffers;

   ++cs->num_buffers;
}

static void
radv_amdgpu_cs_add_buffer(struct ac_cmdbuf *_cs, struct radeon_winsys_bo *_bo)
{
   struct radv_amdgpu_cs *cs = radv_amdgpu_cs(_cs);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);

   if (cs->base.status != VK_SUCCESS)
      return;

   if (bo->base.is_virtual)
      return;

   radv_amdgpu_cs_add_buffer_internal(cs, bo->base.handle, bo->priority);
}

static void
radv_amdgpu_cs_execute_secondary(struct ac_cmdbuf *_parent, struct ac_cmdbuf *_child, bool allow_ib2)
{
   struct radv_amdgpu_cs *parent = radv_amdgpu_cs(_parent);
   struct radv_amdgpu_cs *child = radv_amdgpu_cs(_child);

   if (parent->base.status != VK_SUCCESS || child->base.status != VK_SUCCESS)
      return;

   for (unsigned i = 0; i < child->num_buffers; ++i) {
      radv_amdgpu_cs_add_buffer_internal(parent, child->handles[i].bo_handle, child->handles[i].bo_priority);
   }

   radv_winsys_cs_execute_secondary(&parent->base, &child->base, allow_ib2);
}

static unsigned
radv_amdgpu_count_cs_bo(struct radv_amdgpu_cs *start_cs)
{
   unsigned num_bo = 0;

   for (struct radv_amdgpu_cs *cs = start_cs; cs; cs = cs->chained_to) {
      num_bo += cs->num_buffers;
   }

   return num_bo;
}

static unsigned
radv_amdgpu_count_cs_array_bo(struct ac_cmdbuf **cs_array, unsigned num_cs)
{
   unsigned num_bo = 0;

   for (unsigned i = 0; i < num_cs; ++i) {
      num_bo += radv_amdgpu_count_cs_bo(radv_amdgpu_cs(cs_array[i]));
   }

   return num_bo;
}

static unsigned
radv_amdgpu_add_cs_to_bo_list(struct radv_amdgpu_cs *cs, struct drm_amdgpu_bo_list_entry *handles, unsigned num_handles)
{
   if (!cs->num_buffers)
      return num_handles;

   if (num_handles == 0) {
      memcpy(handles, cs->handles, cs->num_buffers * sizeof(struct drm_amdgpu_bo_list_entry));
      return cs->num_buffers;
   }

   int unique_bo_so_far = num_handles;
   for (unsigned j = 0; j < cs->num_buffers; ++j) {
      bool found = false;
      for (unsigned k = 0; k < unique_bo_so_far; ++k) {
         if (handles[k].bo_handle == cs->handles[j].bo_handle) {
            found = true;
            break;
         }
      }
      if (!found) {
         handles[num_handles] = cs->handles[j];
         ++num_handles;
      }
   }

   return num_handles;
}

static unsigned
radv_amdgpu_add_cs_array_to_bo_list(struct ac_cmdbuf **cs_array, unsigned num_cs,
                                    struct drm_amdgpu_bo_list_entry *handles, unsigned num_handles)
{
   for (unsigned i = 0; i < num_cs; ++i) {
      for (struct radv_amdgpu_cs *cs = radv_amdgpu_cs(cs_array[i]); cs; cs = cs->chained_to) {
         num_handles = radv_amdgpu_add_cs_to_bo_list(cs, handles, num_handles);
      }
   }

   return num_handles;
}

static unsigned
radv_amdgpu_copy_global_bo_list(struct radv_amdgpu_winsys *ws, struct drm_amdgpu_bo_list_entry *handles)
{
   for (uint32_t i = 0; i < ws->global_bo_list.count; i++) {
      handles[i].bo_handle = ws->global_bo_list.bos[i]->handle;
      handles[i].bo_priority = radv_amdgpu_winsys_bo(ws->global_bo_list.bos[i])->priority;
   }

   return ws->global_bo_list.count;
}

static VkResult
radv_amdgpu_get_bo_list(struct radv_amdgpu_winsys *ws, struct ac_cmdbuf **cs_array, unsigned count,
                        struct ac_cmdbuf **initial_preamble_array, unsigned num_initial_preambles,
                        struct ac_cmdbuf **continue_preamble_array, unsigned num_continue_preambles,
                        struct ac_cmdbuf **postamble_array, unsigned num_postambles, unsigned *rnum_handles,
                        struct drm_amdgpu_bo_list_entry **rhandles)
{
   struct drm_amdgpu_bo_list_entry *handles = NULL;
   unsigned num_handles = 0;

   if (ws->debug_all_bos) {
      handles = malloc(sizeof(handles[0]) * ws->global_bo_list.count);
      if (!handles)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      num_handles = radv_amdgpu_copy_global_bo_list(ws, handles);
   } else if (count == 1 && !num_initial_preambles && !num_continue_preambles && !num_postambles &&
              !radv_amdgpu_cs(cs_array[0])->chained_to && !ws->global_bo_list.count) {
      struct radv_amdgpu_cs *cs = (struct radv_amdgpu_cs *)cs_array[0];
      if (cs->num_buffers == 0)
         return VK_SUCCESS;

      handles = malloc(sizeof(handles[0]) * cs->num_buffers);
      if (!handles)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      memcpy(handles, cs->handles, sizeof(handles[0]) * cs->num_buffers);
      num_handles = cs->num_buffers;
   } else {
      unsigned total_buffer_count = ws->global_bo_list.count;
      total_buffer_count += radv_amdgpu_count_cs_array_bo(cs_array, count);
      total_buffer_count += radv_amdgpu_count_cs_array_bo(initial_preamble_array, num_initial_preambles);
      total_buffer_count += radv_amdgpu_count_cs_array_bo(continue_preamble_array, num_continue_preambles);
      total_buffer_count += radv_amdgpu_count_cs_array_bo(postamble_array, num_postambles);

      if (total_buffer_count == 0)
         return VK_SUCCESS;

      handles = malloc(sizeof(handles[0]) * total_buffer_count);
      if (!handles)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      num_handles = radv_amdgpu_copy_global_bo_list(ws, handles);
      num_handles = radv_amdgpu_add_cs_array_to_bo_list(cs_array, count, handles, num_handles);
      num_handles =
         radv_amdgpu_add_cs_array_to_bo_list(initial_preamble_array, num_initial_preambles, handles, num_handles);
      num_handles =
         radv_amdgpu_add_cs_array_to_bo_list(continue_preamble_array, num_continue_preambles, handles, num_handles);
      num_handles = radv_amdgpu_add_cs_array_to_bo_list(postamble_array, num_postambles, handles, num_handles);
   }

   *rhandles = handles;
   *rnum_handles = num_handles;

   return VK_SUCCESS;
}

static void
radv_assign_last_submit(struct radv_amdgpu_ctx *ctx, struct radv_amdgpu_cs_request *request)
{
   radv_amdgpu_request_to_fence(ctx, &ctx->last_submission[request->ip_type][request->ring], request);
}

static unsigned
radv_amdgpu_submitted_ibs_per_cs(const struct radv_amdgpu_cs *cs)
{
   return cs->chain_ib ? 1 : cs->num_ib_buffers;
}

static unsigned
radv_amdgpu_count_submitted_ibs(struct ac_cmdbuf **cs_array, unsigned cs_count, unsigned initial_preamble_count,
                                unsigned continue_preamble_count, unsigned postamble_count)
{
   unsigned num_ibs = 0;

   for (unsigned i = 0; i < cs_count; i++) {
      struct radv_amdgpu_cs *cs = radv_amdgpu_cs(cs_array[i]);

      num_ibs += radv_amdgpu_submitted_ibs_per_cs(cs);
   }

   return MAX2(initial_preamble_count, continue_preamble_count) + num_ibs + postamble_count;
}

static VkResult
radv_amdgpu_winsys_cs_submit_internal(struct radv_amdgpu_ctx *ctx, int queue_idx, struct radv_winsys_sem_info *sem_info,
                                      struct ac_cmdbuf **cs_array, unsigned cs_count,
                                      struct ac_cmdbuf **initial_preamble_cs, unsigned initial_preamble_count,
                                      struct ac_cmdbuf **continue_preamble_cs, unsigned continue_preamble_count,
                                      struct ac_cmdbuf **postamble_cs, unsigned postamble_count, bool uses_shadow_regs)
{
   VkResult result;

   /* Last CS is "the gang leader", its IP type determines which fence to signal. */
   struct radv_amdgpu_cs *last_cs = radv_amdgpu_cs(cs_array[cs_count - 1]);
   struct radv_amdgpu_winsys *ws = last_cs->ws;

   const unsigned num_ibs = radv_amdgpu_count_submitted_ibs(cs_array, cs_count, initial_preamble_count,
                                                            continue_preamble_count, postamble_count);
   const unsigned ib_array_size = MIN2(RADV_MAX_IBS_PER_SUBMIT, num_ibs);

   STACK_ARRAY(struct radv_amdgpu_cs_ib_info, ibs, ib_array_size);

   struct drm_amdgpu_bo_list_entry *handles = NULL;
   unsigned num_handles = 0;

   u_rwlock_rdlock(&ws->global_bo_list.lock);

   result = radv_amdgpu_get_bo_list(ws, &cs_array[0], cs_count, initial_preamble_cs, initial_preamble_count,
                                    continue_preamble_cs, continue_preamble_count, postamble_cs, postamble_count,
                                    &num_handles, &handles);
   if (result != VK_SUCCESS)
      goto fail;

   /* Configure the CS request. */
   const uint32_t *max_ib_per_ip = ws->info.max_submitted_ibs;
   struct radv_amdgpu_cs_request request = {
      .ip_type = last_cs->hw_ip,
      .ip_instance = 0,
      .ring = queue_idx,
      .handles = handles,
      .num_handles = num_handles,
      .ibs = ibs,
      .number_of_ibs = 0, /* set below */
   };

   for (unsigned cs_idx = 0, cs_ib_idx = 0; cs_idx < cs_count;) {
      struct ac_cmdbuf **preambles = cs_idx ? continue_preamble_cs : initial_preamble_cs;
      const unsigned preamble_count = cs_idx ? continue_preamble_count : initial_preamble_count;
      const unsigned ib_per_submit = RADV_MAX_IBS_PER_SUBMIT - preamble_count - postamble_count;
      unsigned num_submitted_ibs = 0;
      unsigned ibs_per_ip[AMD_NUM_IP_TYPES] = {0};

      /* Copy preambles to the submission. */
      for (unsigned i = 0; i < preamble_count; ++i) {
         /* Assume that the full preamble fits into 1 IB. */
         struct radv_winsys_cs *cs = radv_winsys_cs(preambles[i]);
         struct radv_amdgpu_cs_ib_info ib;

         if (ws->dump_ibs)
            ws->base.cs_dump(&cs->base, stderr, NULL, 0, RADV_CS_DUMP_TYPE_PREAMBLE_IBS);

         assert(cs->num_ib_buffers == 1);
         ib = radv_winsys_cs_ib_to_info(cs, cs->ib_buffers[0]);

         ibs[num_submitted_ibs++] = ib;
         ibs_per_ip[cs->hw_ip]++;
      }

      for (unsigned i = 0; i < ib_per_submit && cs_idx < cs_count; ++i) {
         struct radv_winsys_cs *cs = radv_winsys_cs(cs_array[cs_idx]);
         struct radv_amdgpu_cs_ib_info ib;

         if (ws->dump_ibs)
            ws->base.cs_dump(&cs->base, stderr, NULL, 0, RADV_CS_DUMP_TYPE_MAIN_IBS);

         if (cs_ib_idx == 0) {
            /* Make sure the whole CS fits into the same submission. */
            unsigned cs_num_ib = radv_amdgpu_submitted_ibs_per_cs(cs);
            if (i + cs_num_ib > ib_per_submit || ibs_per_ip[cs->hw_ip] + cs_num_ib > max_ib_per_ip[cs->hw_ip])
               break;

            if (cs->hw_ip != request.ip_type) {
               /* Found a "follower" CS in a gang submission.
                * Make sure to submit this together with its "leader", the next CS.
                * We rely on the caller to order each "follower" before its "leader."
                */
               assert(cs_idx != cs_count - 1);
               struct radv_winsys_cs *next_cs = radv_winsys_cs(cs_array[cs_idx + 1]);
               assert(next_cs->hw_ip == request.ip_type);
               unsigned next_cs_num_ib = radv_amdgpu_submitted_ibs_per_cs(next_cs);
               if (i + cs_num_ib + next_cs_num_ib > ib_per_submit ||
                   ibs_per_ip[next_cs->hw_ip] + next_cs_num_ib > max_ib_per_ip[next_cs->hw_ip])
                  break;
            }
         }

         /* When IBs are used, we only need to submit the main IB of this CS, because everything
          * else is chained to the first IB. Otherwise we must submit all IBs in the ib_buffers
          * array.
          */
         if (cs->chain_ib) {
            ib = radv_winsys_cs_ib_to_info(cs, cs->ib_buffers[0]);
            cs_idx++;
         } else {
            assert(cs_ib_idx < cs->num_ib_buffers);
            ib = radv_winsys_cs_ib_to_info(cs, cs->ib_buffers[cs_ib_idx++]);

            if (cs_ib_idx == cs->num_ib_buffers) {
               cs_idx++;
               cs_ib_idx = 0;
            }
         }

         if (uses_shadow_regs && ib.ip_type == AMDGPU_HW_IP_GFX)
            ib.flags |= AMDGPU_IB_FLAG_PREEMPT;

         assert(num_submitted_ibs < ib_array_size);
         ibs[num_submitted_ibs++] = ib;
         ibs_per_ip[cs->hw_ip]++;
      }

      assert(num_submitted_ibs > preamble_count);

      /* Copy postambles to the submission. */
      for (unsigned i = 0; i < postamble_count; ++i) {
         /* Assume that the full postamble fits into 1 IB. */
         struct radv_amdgpu_cs *cs = radv_amdgpu_cs(postamble_cs[i]);
         struct radv_amdgpu_cs_ib_info ib;

         if (ws->dump_ibs)
            ws->base.cs_dump(&cs->base, stderr, NULL, 0, RADV_CS_DUMP_TYPE_POSTAMBLE_IBS);

         assert(cs->num_ib_buffers == 1);
         ib = radv_amdgpu_cs_ib_to_info(cs, cs->ib_buffers[0]);

         ibs[num_submitted_ibs++] = ib;
         ibs_per_ip[cs->hw_ip]++;
      }

      /* Submit the CS. */
      request.number_of_ibs = num_submitted_ibs;
      result = radv_amdgpu_cs_submit(ctx, &request, sem_info);
      if (result != VK_SUCCESS)
         goto fail;
   }

   free(request.handles);

   if (result != VK_SUCCESS)
      goto fail;

   radv_assign_last_submit(ctx, &request);

fail:
   u_rwlock_rdunlock(&ws->global_bo_list.lock);
   STACK_ARRAY_FINISH(ibs);
   return result;
}

static VkResult
radv_amdgpu_cs_submit_zero(struct radv_amdgpu_ctx *ctx, enum amd_ip_type ip_type, int queue_idx,
                           struct radv_winsys_sem_info *sem_info)
{
   unsigned hw_ip = ip_type;
   unsigned queue_syncobj = radv_amdgpu_ctx_queue_syncobj(ctx, hw_ip, queue_idx);
   int ret;

   if (!queue_syncobj)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (sem_info->wait.syncobj_count || sem_info->wait.timeline_syncobj_count) {
      int fd;
      ret = ac_drm_cs_syncobj_export_sync_file(ctx->ws->dev, queue_syncobj, &fd);
      if (ret < 0)
         return VK_ERROR_DEVICE_LOST;

      for (unsigned i = 0; i < sem_info->wait.syncobj_count; ++i) {
         int fd2;
         ret = ac_drm_cs_syncobj_export_sync_file(ctx->ws->dev, sem_info->wait.syncobj[i], &fd2);
         if (ret < 0) {
            close(fd);
            return VK_ERROR_DEVICE_LOST;
         }

         sync_accumulate("radv", &fd, fd2);
         close(fd2);
      }
      for (unsigned i = 0; i < sem_info->wait.timeline_syncobj_count; ++i) {
         int fd2;
         ret = ac_drm_cs_syncobj_export_sync_file2(
            ctx->ws->dev, sem_info->wait.syncobj[i + sem_info->wait.syncobj_count], sem_info->wait.points[i], 0, &fd2);
         if (ret < 0) {
            /* This works around a kernel bug where the fence isn't copied if it is already
             * signalled. Since it is already signalled it is totally fine to not wait on it.
             *
             * kernel patch: https://patchwork.freedesktop.org/patch/465583/ */
            uint64_t point;
            ret = ac_drm_cs_syncobj_query2(ctx->ws->dev, &sem_info->wait.syncobj[i + sem_info->wait.syncobj_count],
                                           &point, 1, 0);
            if (!ret && point >= sem_info->wait.points[i])
               continue;

            close(fd);
            return VK_ERROR_DEVICE_LOST;
         }

         sync_accumulate("radv", &fd, fd2);
         close(fd2);
      }
      ret = ac_drm_cs_syncobj_import_sync_file(ctx->ws->dev, queue_syncobj, fd);
      close(fd);
      if (ret < 0)
         return VK_ERROR_DEVICE_LOST;

      ctx->queue_syncobj_wait[hw_ip][queue_idx] = true;
   }

   for (unsigned i = 0; i < sem_info->signal.syncobj_count; ++i) {
      uint32_t dst_handle = sem_info->signal.syncobj[i];
      uint32_t src_handle = queue_syncobj;

      if (ctx->ws->info.has_timeline_syncobj) {
         ret = ac_drm_cs_syncobj_transfer(ctx->ws->dev, dst_handle, 0, src_handle, 0, 0);
         if (ret < 0)
            return VK_ERROR_DEVICE_LOST;
      } else {
         int fd;
         ret = ac_drm_cs_syncobj_export_sync_file(ctx->ws->dev, src_handle, &fd);
         if (ret < 0)
            return VK_ERROR_DEVICE_LOST;

         ret = ac_drm_cs_syncobj_import_sync_file(ctx->ws->dev, dst_handle, fd);
         close(fd);
         if (ret < 0)
            return VK_ERROR_DEVICE_LOST;
      }
   }
   for (unsigned i = 0; i < sem_info->signal.timeline_syncobj_count; ++i) {
      ret = ac_drm_cs_syncobj_transfer(ctx->ws->dev, sem_info->signal.syncobj[i + sem_info->signal.syncobj_count],
                                       sem_info->signal.points[i], queue_syncobj, 0, 0);
      if (ret < 0)
         return VK_ERROR_DEVICE_LOST;
   }
   return VK_SUCCESS;
}

static VkResult
radv_amdgpu_winsys_cs_submit(struct radeon_winsys_ctx *_ctx, const struct radv_winsys_submit_info *submit,
                             uint32_t wait_count, const struct vk_sync_wait *waits, uint32_t signal_count,
                             const struct vk_sync_signal *signals)
{
   struct radv_amdgpu_ctx *ctx = radv_amdgpu_ctx(_ctx);
   struct radv_amdgpu_winsys *ws = ctx->ws;
   VkResult result;
   unsigned wait_idx = 0, signal_idx = 0;

   STACK_ARRAY(uint64_t, wait_points, wait_count);
   STACK_ARRAY(uint32_t, wait_syncobj, wait_count);
   STACK_ARRAY(uint64_t, signal_points, signal_count);
   STACK_ARRAY(uint32_t, signal_syncobj, signal_count);

   if (!wait_points || !wait_syncobj || !signal_points || !signal_syncobj) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   for (uint32_t i = 0; i < wait_count; ++i) {
      if (waits[i].sync->type == &vk_sync_dummy_type)
         continue;

      assert(waits[i].sync->type == &ws->syncobj_sync_type);
      wait_syncobj[wait_idx] = ((struct vk_drm_syncobj *)waits[i].sync)->syncobj;
      wait_points[wait_idx] = waits[i].wait_value;
      ++wait_idx;
   }

   for (uint32_t i = 0; i < signal_count; ++i) {
      if (signals[i].sync->type == &vk_sync_dummy_type)
         continue;

      assert(signals[i].sync->type == &ws->syncobj_sync_type);
      signal_syncobj[signal_idx] = ((struct vk_drm_syncobj *)signals[i].sync)->syncobj;
      signal_points[signal_idx] = signals[i].signal_value;
      ++signal_idx;
   }

   assert(signal_idx <= signal_count);
   assert(wait_idx <= wait_count);

   const uint32_t wait_timeline_syncobj_count =
      (ws->syncobj_sync_type.features & VK_SYNC_FEATURE_TIMELINE) ? wait_idx : 0;
   const uint32_t signal_timeline_syncobj_count =
      (ws->syncobj_sync_type.features & VK_SYNC_FEATURE_TIMELINE) ? signal_idx : 0;

   struct radv_winsys_sem_info sem_info = {
      .wait =
         {
            .points = wait_points,
            .syncobj = wait_syncobj,
            .timeline_syncobj_count = wait_timeline_syncobj_count,
            .syncobj_count = wait_idx - wait_timeline_syncobj_count,
         },
      .signal =
         {
            .points = signal_points,
            .syncobj = signal_syncobj,
            .timeline_syncobj_count = signal_timeline_syncobj_count,
            .syncobj_count = signal_idx - signal_timeline_syncobj_count,
         },
      .cs_emit_wait = true,
      .cs_emit_signal = true,
   };

   if (!submit->cs_count) {
      result = radv_amdgpu_cs_submit_zero(ctx, submit->ip_type, submit->queue_index, &sem_info);
   } else {
      result = radv_amdgpu_winsys_cs_submit_internal(
         ctx, submit->queue_index, &sem_info, submit->cs_array, submit->cs_count, submit->initial_preamble_cs,
         submit->initial_preamble_count, submit->continue_preamble_cs, submit->continue_preamble_count,
         submit->postamble_cs, submit->postamble_count, submit->uses_shadow_regs);
   }

out:
   STACK_ARRAY_FINISH(wait_points);
   STACK_ARRAY_FINISH(wait_syncobj);
   STACK_ARRAY_FINISH(signal_points);
   STACK_ARRAY_FINISH(signal_syncobj);
   return result;
}

static void
radv_amdgpu_winsys_get_cpu_addr(void *_cs, uint64_t addr, struct ac_addr_info *info)
{
   struct radv_amdgpu_cs *cs = (struct radv_amdgpu_cs *)_cs;

   memset(info, 0, sizeof(struct ac_addr_info));

   if (cs->ws->debug_log_bos) {
      bool destroyed = false;

      if (radv_winsys_bo_log_find(&cs->ws->bo_log, addr, &destroyed))
         info->use_after_free = destroyed;
   }

   if (info->use_after_free)
      return;

   info->valid = !cs->ws->debug_all_bos;

   if (radv_winsys_cs_get_cpu_addr(&cs->base, addr, &info->cpu_addr)) {
      info->valid = true;
      return;
   }

   if (radv_winsys_bo_list_get_cpu_addr(&cs->ws->global_bo_list, &cs->ws->base, addr, &info->cpu_addr)) {
      info->valid = true;
      return;
   }

   return;
}

static uint32_t
radv_to_amdgpu_priority(enum radeon_ctx_priority radv_priority)
{
   switch (radv_priority) {
   case RADEON_CTX_PRIORITY_REALTIME:
      return AMDGPU_CTX_PRIORITY_VERY_HIGH;
   case RADEON_CTX_PRIORITY_HIGH:
      return AMDGPU_CTX_PRIORITY_HIGH;
   case RADEON_CTX_PRIORITY_MEDIUM:
      return AMDGPU_CTX_PRIORITY_NORMAL;
   case RADEON_CTX_PRIORITY_LOW:
      return AMDGPU_CTX_PRIORITY_LOW;
   default:
      UNREACHABLE("Invalid context priority");
   }
}

static VkResult
radv_amdgpu_ctx_create(struct radeon_winsys *_ws, enum radeon_ctx_priority priority, struct radeon_winsys_ctx **rctx)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_ctx *ctx = CALLOC_STRUCT(radv_amdgpu_ctx);
   uint32_t amdgpu_priority = radv_to_amdgpu_priority(priority);
   VkResult result;
   int r;

   if (!ctx)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   r = ac_drm_cs_ctx_create2(ws->dev, amdgpu_priority, &ctx->ctx_handle);
   if (r && r == -EACCES) {
      result = VK_ERROR_NOT_PERMITTED;
      goto fail_create;
   } else if (r) {
      fprintf(stderr, "radv/amdgpu: radv_amdgpu_cs_ctx_create2 failed. (%i)\n", r);
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_create;
   }
   ctx->ws = ws;

   assert(AMDGPU_HW_IP_NUM * MAX_RINGS_PER_TYPE * 4 * sizeof(uint64_t) <= 4096);
   result = ws->base.buffer_create(&ws->base, 4096, 8, RADEON_DOMAIN_GTT,
                                   RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING, RADV_BO_PRIORITY_CS, 0,
                                   &ctx->fence_bo);
   if (result != VK_SUCCESS) {
      goto fail_alloc;
   }

   *rctx = (struct radeon_winsys_ctx *)ctx;
   return VK_SUCCESS;

fail_alloc:
   ac_drm_cs_ctx_free(ws->dev, ctx->ctx_handle);
fail_create:
   FREE(ctx);
   return result;
}

static void
radv_amdgpu_ctx_destroy(struct radeon_winsys_ctx *rwctx)
{
   struct radv_amdgpu_ctx *ctx = (struct radv_amdgpu_ctx *)rwctx;

   for (unsigned ip = 0; ip <= AMDGPU_HW_IP_NUM; ++ip) {
      for (unsigned ring = 0; ring < MAX_RINGS_PER_TYPE; ++ring) {
         if (ctx->queue_syncobj[ip][ring])
            ac_drm_cs_destroy_syncobj(ctx->ws->dev, ctx->queue_syncobj[ip][ring]);
      }
   }

   ctx->ws->base.buffer_destroy(&ctx->ws->base, ctx->fence_bo);
   ac_drm_cs_ctx_free(ctx->ws->dev, ctx->ctx_handle);
   FREE(ctx);
}

static VkResult
radv_amdgpu_ctx_is_priority_permitted(struct radeon_winsys *_ws, enum radeon_ctx_priority priority)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   uint32_t amdgpu_priority = radv_to_amdgpu_priority(priority);
   uint32_t ctx_handle;
   int r;

   r = ac_drm_cs_ctx_create2(ws->dev, amdgpu_priority, &ctx_handle);
   if (r && r == -EACCES) {
      return VK_ERROR_NOT_PERMITTED;
   } else if (r) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   ac_drm_cs_ctx_free(ws->dev, ctx_handle);
   return VK_SUCCESS;
}

static uint32_t
radv_amdgpu_ctx_queue_syncobj(struct radv_amdgpu_ctx *ctx, unsigned ip, unsigned ring)
{
   uint32_t *syncobj = &ctx->queue_syncobj[ip][ring];
   if (!*syncobj) {
      ac_drm_cs_create_syncobj2(ctx->ws->dev, DRM_SYNCOBJ_CREATE_SIGNALED, syncobj);
   }
   return *syncobj;
}

static bool
radv_amdgpu_ctx_wait_idle(struct radeon_winsys_ctx *rwctx, enum amd_ip_type ip_type, int ring_index)
{
   struct radv_amdgpu_ctx *ctx = (struct radv_amdgpu_ctx *)rwctx;

   if (ctx->last_submission[ip_type][ring_index].fence.fence) {
      uint32_t expired;
      int ret = ac_drm_cs_query_fence_status(
         ctx->ws->dev, ctx->ctx_handle, ctx->last_submission[ip_type][ring_index].fence.ip_type,
         ctx->last_submission[ip_type][ring_index].fence.ip_instance,
         ctx->last_submission[ip_type][ring_index].fence.ring, ctx->last_submission[ip_type][ring_index].fence.fence,
         1000000000ull, 0, &expired);

      if (ret || !expired)
         return false;
   }

   return true;
}

static uint32_t
radv_to_amdgpu_pstate(enum radeon_ctx_pstate radv_pstate)
{
   switch (radv_pstate) {
   case RADEON_CTX_PSTATE_NONE:
      return AMDGPU_CTX_STABLE_PSTATE_NONE;
   case RADEON_CTX_PSTATE_STANDARD:
      return AMDGPU_CTX_STABLE_PSTATE_STANDARD;
   case RADEON_CTX_PSTATE_MIN_SCLK:
      return AMDGPU_CTX_STABLE_PSTATE_MIN_SCLK;
   case RADEON_CTX_PSTATE_MIN_MCLK:
      return AMDGPU_CTX_STABLE_PSTATE_MIN_MCLK;
   case RADEON_CTX_PSTATE_PEAK:
      return AMDGPU_CTX_STABLE_PSTATE_PEAK;
   default:
      UNREACHABLE("Invalid pstate");
   }
}

static int
radv_amdgpu_ctx_set_pstate(struct radeon_winsys_ctx *rwctx, enum radeon_ctx_pstate pstate)
{
   struct radv_amdgpu_ctx *ctx = (struct radv_amdgpu_ctx *)rwctx;
   uint32_t new_pstate = radv_to_amdgpu_pstate(pstate);
   uint32_t current_pstate = 0;
   int r;

   r = ac_drm_cs_ctx_stable_pstate(ctx->ws->dev, ctx->ctx_handle, AMDGPU_CTX_OP_GET_STABLE_PSTATE, 0, &current_pstate);
   if (r) {
      fprintf(stderr, "radv/amdgpu: failed to get current pstate\n");
      return r;
   }

   /* Do not try to set a new pstate when the current one is already what we want. Otherwise, the
    * kernel might return -EBUSY if we have multiple AMDGPU contexts in flight.
    */
   if (current_pstate == new_pstate)
      return 0;

   r = ac_drm_cs_ctx_stable_pstate(ctx->ws->dev, ctx->ctx_handle, AMDGPU_CTX_OP_SET_STABLE_PSTATE, new_pstate, NULL);
   if (r) {
      fprintf(stderr, "radv/amdgpu: failed to set new pstate\n");
      return r;
   }

   return 0;
}

static void *
radv_amdgpu_cs_alloc_syncobj_chunk(struct radv_winsys_sem_counts *counts, uint32_t queue_syncobj,
                                   struct drm_amdgpu_cs_chunk *chunk, int chunk_id)
{
   unsigned count = counts->syncobj_count + (queue_syncobj ? 1 : 0);
   struct drm_amdgpu_cs_chunk_sem *syncobj = malloc(sizeof(struct drm_amdgpu_cs_chunk_sem) * count);
   if (!syncobj)
      return NULL;

   for (unsigned i = 0; i < counts->syncobj_count; i++) {
      struct drm_amdgpu_cs_chunk_sem *sem = &syncobj[i];
      sem->handle = counts->syncobj[i];
   }

   if (queue_syncobj)
      syncobj[counts->syncobj_count].handle = queue_syncobj;

   chunk->chunk_id = chunk_id;
   chunk->length_dw = sizeof(struct drm_amdgpu_cs_chunk_sem) / 4 * count;
   chunk->chunk_data = (uint64_t)(uintptr_t)syncobj;
   return syncobj;
}

static void *
radv_amdgpu_cs_alloc_timeline_syncobj_chunk(struct radv_winsys_sem_counts *counts, uint32_t queue_syncobj,
                                            struct drm_amdgpu_cs_chunk *chunk, int chunk_id)
{
   uint32_t count = counts->syncobj_count + counts->timeline_syncobj_count + (queue_syncobj ? 1 : 0);
   struct drm_amdgpu_cs_chunk_syncobj *syncobj = malloc(sizeof(struct drm_amdgpu_cs_chunk_syncobj) * count);
   if (!syncobj)
      return NULL;

   for (unsigned i = 0; i < counts->syncobj_count; i++) {
      struct drm_amdgpu_cs_chunk_syncobj *sem = &syncobj[i];
      sem->handle = counts->syncobj[i];
      sem->flags = 0;
      sem->point = 0;
   }

   for (unsigned i = 0; i < counts->timeline_syncobj_count; i++) {
      struct drm_amdgpu_cs_chunk_syncobj *sem = &syncobj[i + counts->syncobj_count];
      sem->handle = counts->syncobj[i + counts->syncobj_count];
      sem->flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
      sem->point = counts->points[i];
   }

   if (queue_syncobj) {
      syncobj[count - 1].handle = queue_syncobj;
      syncobj[count - 1].flags = 0;
      syncobj[count - 1].point = 0;
   }

   chunk->chunk_id = chunk_id;
   chunk->length_dw = sizeof(struct drm_amdgpu_cs_chunk_syncobj) / 4 * count;
   chunk->chunk_data = (uint64_t)(uintptr_t)syncobj;
   return syncobj;
}

static bool
radv_amdgpu_cs_has_user_fence(struct radv_amdgpu_cs_request *request)
{
   return request->ip_type != AMDGPU_HW_IP_UVD && request->ip_type != AMDGPU_HW_IP_VCE &&
          request->ip_type != AMDGPU_HW_IP_UVD_ENC && request->ip_type != AMDGPU_HW_IP_VCN_DEC &&
          request->ip_type != AMDGPU_HW_IP_VCN_ENC && request->ip_type != AMDGPU_HW_IP_VCN_JPEG;
}

static VkResult
radv_amdgpu_cs_submit(struct radv_amdgpu_ctx *ctx, struct radv_amdgpu_cs_request *request,
                      struct radv_winsys_sem_info *sem_info)
{
   int r;
   int num_chunks;
   int size;
   struct drm_amdgpu_cs_chunk *chunks;
   struct drm_amdgpu_cs_chunk_data *chunk_data;
   struct drm_amdgpu_bo_list_in bo_list_in;
   void *wait_syncobj = NULL, *signal_syncobj = NULL;
   int i;
   VkResult result = VK_SUCCESS;
   bool has_user_fence = radv_amdgpu_cs_has_user_fence(request);
   uint32_t queue_syncobj = radv_amdgpu_ctx_queue_syncobj(ctx, request->ip_type, request->ring);
   bool *queue_syncobj_wait = &ctx->queue_syncobj_wait[request->ip_type][request->ring];

   if (!queue_syncobj)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   size = request->number_of_ibs + 1 + (has_user_fence ? 1 : 0) + 1 /* bo list */ + 3;

   chunks = malloc(sizeof(chunks[0]) * size);
   if (!chunks)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   size = request->number_of_ibs + (has_user_fence ? 1 : 0);

   chunk_data = malloc(sizeof(chunk_data[0]) * size);
   if (!chunk_data) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto error_out;
   }

   num_chunks = request->number_of_ibs;
   for (i = 0; i < request->number_of_ibs; i++) {
      struct radv_amdgpu_cs_ib_info *ib;
      chunks[i].chunk_id = AMDGPU_CHUNK_ID_IB;
      chunks[i].length_dw = sizeof(struct drm_amdgpu_cs_chunk_ib) / 4;
      chunks[i].chunk_data = (uint64_t)(uintptr_t)&chunk_data[i];

      ib = &request->ibs[i];
      assert(ib->ib_mc_address && ib->ib_mc_address % ctx->ws->info.ip[ib->ip_type].ib_alignment == 0);
      assert(ib->size);

      chunk_data[i].ib_data._pad = 0;
      chunk_data[i].ib_data.va_start = ib->ib_mc_address;
      chunk_data[i].ib_data.ib_bytes = ib->size * 4;
      chunk_data[i].ib_data.ip_type = ib->ip_type;
      chunk_data[i].ib_data.ip_instance = request->ip_instance;
      chunk_data[i].ib_data.ring = request->ring;
      chunk_data[i].ib_data.flags = ib->flags;
   }

   assert(chunk_data[request->number_of_ibs - 1].ib_data.ip_type == request->ip_type);

   if (has_user_fence) {
      i = num_chunks++;
      chunks[i].chunk_id = AMDGPU_CHUNK_ID_FENCE;
      chunks[i].length_dw = sizeof(struct drm_amdgpu_cs_chunk_fence) / 4;
      chunks[i].chunk_data = (uint64_t)(uintptr_t)&chunk_data[i];

      /* Need to reserve 4 QWORD for user fence:
       *   QWORD[0]: completed fence
       *   QWORD[1]: preempted fence
       *   QWORD[2]: reset fence
       *   QWORD[3]: preempted then reset
       */
      uint32_t offset = (request->ip_type * MAX_RINGS_PER_TYPE + request->ring) * 4;
      ac_drm_cs_chunk_fence_info_to_data(ctx->fence_bo->handle, offset, &chunk_data[i]);
   }

   if (sem_info->cs_emit_wait &&
       (sem_info->wait.timeline_syncobj_count || sem_info->wait.syncobj_count || *queue_syncobj_wait)) {
      uint32_t queue_wait_syncobj = *queue_syncobj_wait ? queue_syncobj : 0;

      if (ctx->ws->info.has_timeline_syncobj) {
         wait_syncobj = radv_amdgpu_cs_alloc_timeline_syncobj_chunk(
            &sem_info->wait, queue_wait_syncobj, &chunks[num_chunks], AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT);
      } else {
         wait_syncobj = radv_amdgpu_cs_alloc_syncobj_chunk(&sem_info->wait, queue_wait_syncobj, &chunks[num_chunks],
                                                           AMDGPU_CHUNK_ID_SYNCOBJ_IN);
      }
      if (!wait_syncobj) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto error_out;
      }
      num_chunks++;

      sem_info->cs_emit_wait = false;
      *queue_syncobj_wait = false;
   }

   if (sem_info->cs_emit_signal) {
      if (ctx->ws->info.has_timeline_syncobj) {
         signal_syncobj = radv_amdgpu_cs_alloc_timeline_syncobj_chunk(
            &sem_info->signal, queue_syncobj, &chunks[num_chunks], AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL);
      } else {
         signal_syncobj = radv_amdgpu_cs_alloc_syncobj_chunk(&sem_info->signal, queue_syncobj, &chunks[num_chunks],
                                                             AMDGPU_CHUNK_ID_SYNCOBJ_OUT);
      }
      if (!signal_syncobj) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto error_out;
      }
      num_chunks++;
   }

   bo_list_in.operation = ~0;
   bo_list_in.list_handle = ~0;
   bo_list_in.bo_number = request->num_handles;
   bo_list_in.bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
   bo_list_in.bo_info_ptr = (uint64_t)(uintptr_t)request->handles;

   chunks[num_chunks].chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES;
   chunks[num_chunks].length_dw = sizeof(struct drm_amdgpu_bo_list_in) / 4;
   chunks[num_chunks].chunk_data = (uintptr_t)&bo_list_in;
   num_chunks++;

   /* The kernel returns -ENOMEM with many parallel processes using GDS such as test suites quite
    * often, but it eventually succeeds after enough attempts. This happens frequently with dEQP
    * using NGG streamout.
    */
   uint64_t abs_timeout_ns = os_time_get_absolute_timeout(1000000000ull); /* 1s */

   r = 0;
   do {
      /* Wait 1 ms and try again. */
      if (r == -ENOMEM)
         os_time_sleep(1000);

      r = ac_drm_cs_submit_raw2(ctx->ws->dev, ctx->ctx_handle, 0, num_chunks, chunks, &request->seq_no);
   } while (r == -ENOMEM && os_time_get_nano() < abs_timeout_ns);

   if (r) {
      if (r == -ENOMEM) {
         fprintf(stderr, "radv/amdgpu: Not enough memory for command submission.\n");
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      } else if (r == -ECANCELED) {
         fprintf(stderr,
                 "radv/amdgpu: The CS has been cancelled because the context is lost. This context is innocent.\n");
         result = VK_ERROR_DEVICE_LOST;
      } else if (r == -ENODATA) {
         fprintf(stderr, "radv/amdgpu: The CS has been cancelled because the context is lost. This context is guilty "
                         "of a soft recovery.\n");
         result = VK_ERROR_DEVICE_LOST;
      } else if (r == -ETIME) {
         fprintf(stderr, "radv/amdgpu: The CS has been cancelled because the context is lost. This context is guilty "
                         "of a hard recovery.\n");
         result = VK_ERROR_DEVICE_LOST;
      } else {
         fprintf(stderr,
                 "radv/amdgpu: The CS has been rejected, "
                 "see dmesg for more information (%i).\n",
                 r);
         result = VK_ERROR_UNKNOWN;
      }
   }

error_out:
   free(chunks);
   free(chunk_data);
   free(wait_syncobj);
   free(signal_syncobj);
   return result;
}

void
radv_amdgpu_cs_init_functions(struct radv_amdgpu_winsys *ws)
{
   ws->base.ctx_create = radv_amdgpu_ctx_create;
   ws->base.ctx_destroy = radv_amdgpu_ctx_destroy;
   ws->base.ctx_is_priority_permitted = radv_amdgpu_ctx_is_priority_permitted;
   ws->base.ctx_wait_idle = radv_amdgpu_ctx_wait_idle;
   ws->base.ctx_set_pstate = radv_amdgpu_ctx_set_pstate;
   ws->base.cs_domain = radv_winsys_cs_domain;
   ws->base.cs_create = radv_amdgpu_cs_create;
   ws->base.cs_destroy = radv_amdgpu_cs_destroy;
   ws->base.cs_grow = radv_winsys_cs_grow;
   ws->base.cs_finalize = radv_winsys_cs_finalize;
   ws->base.cs_reset = radv_amdgpu_cs_reset;
   ws->base.cs_chain = radv_winsys_cs_chain;
   ws->base.cs_unchain = radv_winsys_cs_unchain;
   ws->base.cs_add_buffer = radv_amdgpu_cs_add_buffer;
   ws->base.cs_execute_secondary = radv_amdgpu_cs_execute_secondary;
   ws->base.cs_execute_ib = radv_winsys_cs_execute_ib;
   ws->base.cs_chain_dgc_ib = radv_winsys_cs_chain_dgc_ib;
   ws->base.cs_submit = radv_amdgpu_winsys_cs_submit;
   ws->base.cs_dump = radv_winsys_cs_dump;
   ws->base.cs_get_cpu_addr = radv_amdgpu_winsys_get_cpu_addr;
   ws->base.cs_annotate = radv_winsys_cs_annotate;
   ws->base.cs_pad = radv_winsys_cs_pad;
}
