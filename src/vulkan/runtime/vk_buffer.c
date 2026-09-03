/*
 * Copyright © 2022 Collabora, Ltd.
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

#include "vk_buffer.h"

#ifdef WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_common_entrypoints.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_util.h"

void
vk_buffer_init(struct vk_device *device,
               struct vk_buffer *buffer,
               const VkBufferCreateInfo *pCreateInfo)
{
   vk_object_base_init(device, &buffer->base, VK_OBJECT_TYPE_BUFFER);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
   assert(pCreateInfo->size > 0);

   buffer->create_flags = pCreateInfo->flags;
   buffer->size = pCreateInfo->size;
   buffer->usage = pCreateInfo->usage;
   buffer->device_address = 0;

   const VkBufferUsageFlags2CreateInfoKHR *usage2_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR);
   if (usage2_info != NULL)
      buffer->usage = usage2_info->usage;

   buffer->address_flags =
      ((buffer->create_flags & VK_BUFFER_CREATE_PROTECTED_BIT) ?
       VK_ADDRESS_COMMAND_PROTECTED_BIT_KHR : 0) |
      ((buffer->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ?
       VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR : 0) |
      ((buffer->usage & VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT) ?
       VK_ADDRESS_COMMAND_TRANSFORM_FEEDBACK_BUFFER_USAGE_BIT_KHR : 0) |
      ((buffer->usage & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) == 0 ?
       VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR : 0);

   buffer->copy_flags =
      ((buffer->create_flags & VK_BUFFER_CREATE_PROTECTED_BIT) ?
       VK_ADDRESS_COPY_PROTECTED_BIT_KHR : 0) |
      ((buffer->create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) ?
       VK_ADDRESS_COPY_SPARSE_BIT_KHR : 0);
}

void *
vk_buffer_create(struct vk_device *device,
                 const VkBufferCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *alloc,
                 size_t size)
{
   struct vk_buffer *buffer =
      vk_zalloc2(&device->alloc, alloc, size, 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (buffer == NULL)
      return NULL;

   vk_buffer_init(device, buffer, pCreateInfo);

   return buffer;
}

void
vk_buffer_finish(struct vk_buffer *buffer)
{
   vk_object_base_finish(&buffer->base);
}

void
vk_buffer_destroy(struct vk_device *device,
                  const VkAllocationCallbacks *alloc,
                  struct vk_buffer *buffer)
{
   vk_object_free(device, alloc, buffer);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_GetBufferMemoryRequirements(VkDevice _device,
                                      VkBuffer buffer,
                                      VkMemoryRequirements *pMemoryRequirements)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   VkBufferMemoryRequirementsInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
      .buffer = buffer,
   };
   VkMemoryRequirements2 reqs = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };
   device->dispatch_table.GetBufferMemoryRequirements2(_device, &info, &reqs);

   *pMemoryRequirements = reqs.memoryRequirements;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_GetBufferMemoryRequirements2(VkDevice _device,
                                       const VkBufferMemoryRequirementsInfo2 *pInfo,
                                       VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   struct vk_buffer *buffer;

#ifdef WIN32
   /* Defensive guard: the public GetBufferMemoryRequirements2 entrypoint may be
     * called (by the app's middleware) with a stale / device-address-alias VkBuffer
     * handle.  Dereferencing such a handle directly faults (observed on RADV-WDDM2
     * under DOOM: The Dark Ages with emulate_rt).  Probe the handle with
     * VirtualQuery (which never faults on a bad pointer) and only dereference it
     * when it is a readable committed user page of the correct object type;
     * otherwise log and return zeroed requirements instead of crashing.
     */
   {
      const struct vk_object_base *base = (const struct vk_object_base *)(uintptr_t)pInfo->buffer;
      MEMORY_BASIC_INFORMATION mbi;
      int valid = 0;

      if (base != NULL &&
          VirtualQuery(base, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT &&
          (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                          PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0 &&
          mbi.RegionSize >= offsetof(struct vk_object_base, type) + sizeof(base->type) &&
          base->type == VK_OBJECT_TYPE_BUFFER)
         valid = 1;

      if (!valid) {
         static int warn = -1;
         if (warn < 0)
            warn = getenv("RADV_TRACE_GBMR2") ? 1 : 0;
         if (warn)
            fprintf(stderr,
                    "RADV GBMR2: rejecting invalid VkBuffer handle %p "
                    "(device=%p thread=%08lx region=%p sz=0x%zx state=0x%lx prot=0x%lx)\n",
                    (void *)pInfo->buffer, (void *)_device, (unsigned long)GetCurrentThreadId(),
                    mbi.BaseAddress, (size_t)mbi.RegionSize, (unsigned long)mbi.State,
                    (unsigned long)mbi.Protect);

         memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
         pMemoryRequirements->sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
         return;
      }

      buffer = (struct vk_buffer *)base;
   }
#else
   buffer = vk_buffer_from_handle(pInfo->buffer);
#endif

   VkBufferUsageFlags2CreateInfoKHR usage2_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
      .pNext = NULL,
      .usage = buffer->usage,
   };
   VkBufferCreateInfo pCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = &usage2_info,
      .usage = buffer->usage,
      .size = buffer->size,
      .flags = buffer->create_flags,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = 0,
      .pQueueFamilyIndices = NULL,
   };
   VkDeviceBufferMemoryRequirements info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pNext = NULL,
      .pCreateInfo = &pCreateInfo,
   };

   device->dispatch_table.GetDeviceBufferMemoryRequirements(_device, &info, pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_BindBufferMemory(VkDevice _device,
                           VkBuffer buffer,
                           VkDeviceMemory memory,
                           VkDeviceSize memoryOffset)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   VkBindBufferMemoryInfo bind = {
      .sType         = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer        = buffer,
      .memory        = memory,
      .memoryOffset  = memoryOffset,
   };

   return device->dispatch_table.BindBufferMemory2(_device, 1, &bind);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
vk_common_GetBufferDeviceAddress(UNUSED VkDevice device,
                                 const VkBufferDeviceAddressInfo *pInfo)
{
   VK_FROM_HANDLE(vk_buffer, buffer, pInfo->buffer);

   return buffer->device_address;
}
