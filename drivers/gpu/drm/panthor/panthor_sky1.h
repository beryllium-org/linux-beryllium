/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2024 Cix Technology Group Co., Ltd. */

#ifndef __PANTHOR_SKY1_H__
#define __PANTHOR_SKY1_H__

struct panthor_device;

#if IS_ENABLED(CONFIG_ACPI)

bool panthor_is_sky1(struct panthor_device *ptdev);
int sky1_gpu_power_on(struct panthor_device *ptdev);
int sky1_hw_power_on(struct panthor_device *ptdev);
int sky1_resets_init(struct panthor_device *ptdev);

#else

static inline bool panthor_is_sky1(struct panthor_device *ptdev)
{
	return false;
}

static inline int sky1_gpu_power_on(struct panthor_device *ptdev)
{
	return 0;
}

static inline int sky1_hw_power_on(struct panthor_device *ptdev)
{
	return 0;
}

static inline int sky1_resets_init(struct panthor_device *ptdev)
{
	return 0;
}

#endif

#endif /* __PANTHOR_SKY1_H__ */
