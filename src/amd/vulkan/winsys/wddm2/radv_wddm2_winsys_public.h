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

#ifndef RADV_WDDM2_WINSYS_PUBLIC_H
#define RADV_WDDM2_WINSYS_PUBLIC_H

#include <vulkan/vulkan_core.h>
#include "ac_gpu_info.h"
#include "vk_sync.h"

struct radeon_winsys;
struct vk_dx_adapter_info;

struct radeon_winsys_info {
   struct radeon_info base;
   struct vk_sync_type syncobj_sync_type;
   uint32_t global_priority_mask;
};

VkResult radv_wddm2_winsys_query_info(const struct vk_dx_adapter_info *adapter_info,
                                      uint64_t debug_flags, struct radeon_winsys_info *info);

VkResult radv_wddm2_winsys_create(const struct vk_dx_adapter_info *adapter_info,
                                  const struct radeon_info *info, uint64_t debug_flags,
                                  uint64_t perftest_flags, struct radeon_winsys **winsys);

const struct vk_sync_type *const *
radv_wddm2_winsys_get_sync_types(struct radeon_winsys *ws);

#endif /* RADV_WDDM2_WINSYS_PUBLIC_H */
