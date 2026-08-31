// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#ifndef __KVM_DEVICE_H
#define __KVM_DEVICE_H

#include <asm/kvm_host.h>

struct pkvm_device_ops {
	int (*reset)(void *cookie, bool host_to_guest);
	int (*power_lock)(void *cookie, bool lock);
};

/*
 * @base: physical address of the resource.
 * @size: size of resource in bytes.
 * @flags: PKVM_DEV_RESOURCE_* flags.
 * @mapped: whether a shared resource is currently mapped by its guest.
 */
struct pkvm_dev_resource {
	u64 base;
	u64 size;
	u32 flags;
	bool mapped;
};

/* The host and guest intentionally share this device-backed aperture. */
#define PKVM_DEV_RESOURCE_SHARED	1U

/*
 * @id: hypervisor ID of the IOMMU as defined by the driver.
 * @endpoint: endpoint ID of the device.
 */
struct pkvm_dev_iommu {
	u64 id;
	u64 endpoint;
};

#define PKVM_DEVICE_MAX_RESOURCE	32
#define PKVM_DEVICE_MAX_IOMMU		32

struct pkvm_device {
	struct pkvm_dev_resource resources[PKVM_DEVICE_MAX_RESOURCE];
	struct pkvm_dev_iommu iommus[PKVM_DEVICE_MAX_IOMMU];
	u32 nr_resources;
	u32 nr_iommus;
	u32 group_id;
	void *ctxt; /* Current context of the device */
	unsigned short refcount;
	bool power_locked;
	struct pkvm_device_ops *ops;
	void *cookie; /* cookie from drivers. */
};

struct pkvm_hyp_vm;
struct pkvm_hyp_vcpu;
int pkvm_devices_get_context(u64 iommu_id, u32 endpoint_id,
			     struct pkvm_hyp_vm *vm);
void pkvm_devices_put_context(u64 iommu_id, u32 endpoint_id);
bool pkvm_device_is_shared_resource(struct pkvm_hyp_vm *vm, u64 phys,
				    size_t size);
int pkvm_init_devices(void);
int pkvm_device_hyp_assign_mmio(u64 pfn, u64 nr_pages);
int pkvm_device_reclaim_mmio(u64 pfn, u64 nr_pages);
int pkvm_host_map_guest_mmio(struct pkvm_hyp_vcpu *hyp_vcpu,
			     u64 pfn, u64 gfn);
bool pkvm_device_request_mmio(struct pkvm_hyp_vcpu *hyp_vcpu,
			      u64 *exit_code);
bool pkvm_device_request_dma(struct pkvm_hyp_vcpu *hyp_vcpu,
			     u64 *exit_code);
bool pkvm_device_request_power(struct pkvm_hyp_vcpu *hyp_vcpu,
			       u64 *exit_code);
void pkvm_device_request_power_pvm_entry(struct pkvm_hyp_vcpu *hyp_vcpu);
void pkvm_devices_teardown(struct pkvm_hyp_vm *vm);
int pkvm_device_register_ops(u64 phys, struct pkvm_device_ops *ops,
			     void *cookie);

#endif /* #ifndef __KVM_DEVICE_H */
