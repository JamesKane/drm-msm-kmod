/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 James Kane
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * List Vulkan devices and create a logical device on each.
 */
#include <stdio.h>
#include <stdlib.h>

#include <vulkan/vulkan.h>

int
main(void)
{
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.apiVersion = VK_API_VERSION_1_3,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkPhysicalDeviceProperties props;
	VkPhysicalDevice *pds;
	VkInstance inst;
	VkDevice dev;
	VkResult r;
	uint32_t n, i;
	float prio = 1.0f;

	if ((r = vkCreateInstance(&ici, NULL, &inst)) != VK_SUCCESS) {
		printf("vkCreateInstance: %d\n", r);
		return (1);
	}
	vkEnumeratePhysicalDevices(inst, &n, NULL);
	pds = calloc(n, sizeof(*pds));
	vkEnumeratePhysicalDevices(inst, &n, pds);
	printf("%u device(s)\n", n);
	for (i = 0; i < n; i++) {
		VkDeviceQueueCreateInfo qci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = 0,
			.queueCount = 1,
			.pQueuePriorities = &prio,
		};
		VkDeviceCreateInfo dci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &qci,
		};

		vkGetPhysicalDeviceProperties(pds[i], &props);
		printf("%s: Vulkan %u.%u.%u, driver %#x, type %d\n",
		    props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
		    VK_API_VERSION_MINOR(props.apiVersion),
		    VK_API_VERSION_PATCH(props.apiVersion),
		    props.driverVersion, props.deviceType);
		r = vkCreateDevice(pds[i], &dci, NULL, &dev);
		printf("  vkCreateDevice: %d\n", r);
		if (r == VK_SUCCESS) {
			vkDeviceWaitIdle(dev);
			vkDestroyDevice(dev, NULL);
		}
	}
	vkDestroyInstance(inst, NULL);
	return (0);
}
