/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_VC4

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <vc4_drm.h>
#include <xf86drm.h>

#include "drv_priv.h"
#include "helpers.h"
#include "util.h"

#define VC4_TILING_FORMAT_LINEAR    0
#define VC4_TILING_FORMAT_T         1
#define VC4_TILING_FORMAT_LT        2

static const uint32_t render_target_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_RGB565,
						  DRM_FORMAT_XRGB8888 };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_YVU420 };

static int vc4_init(struct driver *drv)
{
	drv_add_combinations(drv, render_target_formats, ARRAY_SIZE(render_target_formats),
			     &LINEAR_METADATA, BO_USE_RENDER_MASK);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
			     &LINEAR_METADATA, BO_USE_TEXTURE_MASK);
	/*
	 * Chrome uses DMA-buf mmap to write to YV12 buffers, which are then accessed by the
	 * Video Encoder Accelerator (VEA). It could also support NV12 potentially in the future.
	 */
	drv_modify_combination(drv, DRM_FORMAT_YVU420, &LINEAR_METADATA, BO_USE_HW_VIDEO_ENCODER);
	drv_modify_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
			       BO_USE_HW_VIDEO_DECODER | BO_USE_SCANOUT | BO_USE_HW_VIDEO_ENCODER);

	return drv_modify_linear_combinations(drv);
}

static inline uint32_t vc4_utile_width(int bytes_per_pixel)
{
	switch (bytes_per_pixel) {
	case 1:
	case 2:
		return 8;
	case 4:
		return 4;
	case 8:
		return 2;
	default:
		drv_log("Unknown bytes per pixel");
		return 0;
	}
}

static inline uint32_t vc4_utile_height(int bytes_per_pixel)
{
	switch (bytes_per_pixel) {
	case 1:
		return 8;
	case 2:
	case 4:
	case 8:
		return 4;
	default:
		drv_log("Unknown bytes per pixel");
		return 0;
	}
}

static bool vc4_size_is_lt(uint32_t width, uint32_t height, int bytes_per_pixel)
{
	return (width <= 4 * vc4_utile_width(bytes_per_pixel) ||
			height <= 4 * vc4_utile_height(bytes_per_pixel));
}

static int vc4_bo_create_for_modifier(struct bo *bo, uint32_t width, uint32_t height,
				      uint32_t format, uint64_t modifier)
{
	int ret;
	size_t plane;
	uint32_t offset = 0;
	struct drm_vc4_create_bo bo_create;
	struct drm_vc4_set_tiling vc4_set_tiling;

	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		break;
	case DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED:
		break;
	default:
		return -EINVAL;
	}

	bo->meta.format_modifiers[0] = modifier;

	if (modifier == DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED) {
		uint32_t level_width = width;
		uint32_t level_height = height;
		int bytes_per_pixel = drv_bytes_per_pixel_from_format(format, 0);
		uint32_t utile_width = vc4_utile_width(bytes_per_pixel);
		uint32_t utile_height = vc4_utile_height(bytes_per_pixel);

		if (vc4_size_is_lt(width, height, bytes_per_pixel)) {
			bo->meta.tiling = VC4_TILING_FORMAT_LT;

			level_width = ALIGN(level_width, utile_width);
			level_height = ALIGN(level_height, utile_height);
		} else {
			bo->meta.tiling = VC4_TILING_FORMAT_T;

			level_width = ALIGN(level_width, 4 * 2 * utile_width);
			level_height = ALIGN(level_height, 4 * 2 * utile_height);
		}

		bo->meta.strides[0] = (level_width * bytes_per_pixel * MAX(1, 1));
		bo->meta.sizes[0] = level_height * bo->meta.strides[0];
		bo->meta.offsets[0] = bo->meta.sizes[0];

		bo->meta.total_size = bo->meta.sizes[0];

		drv_bo_from_format(bo, bo->meta.strides[0], level_height, format);
	} else {
		/*
		* Since the ARM L1 cache line size is 64 bytes, align to that as a
		* performance optimization.
		*/
		uint32_t stride = drv_stride_from_format(format, width, 0);
		stride = ALIGN(stride, 64);

		bo->meta.tiling = VC4_TILING_FORMAT_LINEAR;

		drv_bo_from_format(bo, stride, height, format);
	}

	memset(&bo_create, 0, sizeof(bo_create));
	bo_create.size = bo->meta.total_size;

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VC4_CREATE_BO, &bo_create);
	if (ret) {
		drv_log("DRM_IOCTL_VC4_CREATE_BO failed (size=%zu)\n", bo->meta.total_size);
		return -errno;
	}

	for (plane = 0; plane < bo->meta.num_planes; plane++)
		bo->handles[plane].u32 = bo_create.handle;

	memset(&vc4_set_tiling, 0, sizeof(vc4_set_tiling));
	vc4_set_tiling.handle = bo->handles[0].u32;
	vc4_set_tiling.modifier = modifier;

	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VC4_SET_TILING, &vc4_set_tiling);
	if (ret) {
		drv_gem_bo_destroy(bo);

		drv_log("DRM_IOCTL_VC4_SET_TILING failed with %d\n", errno);
		return -errno;
	}

	return 0;
}

static int vc4_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			 uint64_t use_flags)
{
	struct combination *combo;

	combo = drv_get_combination(bo->drv, format, use_flags);
	if (!combo)
		return -EINVAL;

	return vc4_bo_create_for_modifier(bo, width, height, format, combo->metadata.modifier);
}

static int vc4_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height,
					uint32_t format, const uint64_t *modifiers, uint32_t count)
{
	static const uint64_t modifier_order[] = {
		DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED,
		DRM_FORMAT_MOD_LINEAR,
	};
	uint64_t modifier;

	modifier = drv_pick_modifier(modifiers, count, modifier_order, ARRAY_SIZE(modifier_order));

	return vc4_bo_create_for_modifier(bo, width, height, format, modifier);
}

static void *vc4_bo_map(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
	int ret;
	struct drm_vc4_mmap_bo bo_map;

	memset(&bo_map, 0, sizeof(bo_map));
	bo_map.handle = bo->handles[0].u32;

	ret = drmCommandWriteRead(bo->drv->fd, DRM_VC4_MMAP_BO, &bo_map, sizeof(bo_map));
	if (ret) {
		drv_log("DRM_VC4_MMAP_BO failed\n");
		return MAP_FAILED;
	}

	vma->length = bo->meta.total_size;
	return mmap(NULL, bo->meta.total_size, drv_get_prot(map_flags), MAP_SHARED, bo->drv->fd,
		    bo_map.offset);
}

const struct backend backend_vc4 = {
	.name = "vc4",
	.init = vc4_init,
	.bo_create = vc4_bo_create,
	.bo_create_with_modifiers = vc4_bo_create_with_modifiers,
	.bo_import = drv_prime_bo_import,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_map = vc4_bo_map,
	.bo_unmap = drv_bo_munmap,
};

#endif
