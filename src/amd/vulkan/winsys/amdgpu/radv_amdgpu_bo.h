/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based on amdgpu winsys.
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_AMDGPU_BO_H
#define RADV_AMDGPU_BO_H

#include "radv_amdgpu_winsys.h"

struct radv_amdgpu_winsys_bo {
   struct radeon_winsys_bo base;
   amdgpu_va_handle va_handle;
   uint32_t flags;
   uint8_t priority;

   ac_drm_bo bo;

   void *cpu_map;
};

static inline struct radv_amdgpu_winsys_bo *
radv_amdgpu_winsys_bo(struct radeon_winsys_bo *bo)
{
   return (struct radv_amdgpu_winsys_bo *)bo;
}

void radv_amdgpu_bo_init_functions(struct radv_amdgpu_winsys *ws);

#endif /* RADV_AMDGPU_BO_H */
