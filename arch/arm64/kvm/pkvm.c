// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 - Google LLC
 * Author: Quentin Perret <qperret@google.com>
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/interval_tree_generic.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kmemleak.h>
#include <linux/kvm_host.h>
#include <asm/kvm_mmu.h>
#include <linux/memblock.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <asm/kvm_pkvm.h>
#include <kvm/device.h>

#include "hyp_constants.h"

DEFINE_STATIC_KEY_FALSE(kvm_protected_mode_initialized);

#define PKVM_DEVICE_ASSIGN_COMPAT "pkvm,device-assignment"
#define PKVM_PCI_DEVICE_ASSIGN_COMPAT "pkvm,pci-device-assignment"
#define PKVM_PCI_PROBE_TIMEOUT_MS 1000
#define PKVM_PCI_PROBE_INTERVAL_MS 10

extern struct pkvm_device *kvm_nvhe_sym(registered_devices);
extern unsigned long kvm_nvhe_sym(registered_devices_nr);

static struct memblock_region *hyp_memory = kvm_nvhe_sym(hyp_memory);
static unsigned int *hyp_memblock_nr_ptr = &kvm_nvhe_sym(hyp_memblock_nr);

phys_addr_t hyp_mem_base;
phys_addr_t hyp_mem_size;

static int __init register_memblock_regions(void)
{
	struct memblock_region *reg;

	for_each_mem_region(reg) {
		if (*hyp_memblock_nr_ptr >= HYP_MEMBLOCK_REGIONS)
			return -ENOMEM;

		hyp_memory[*hyp_memblock_nr_ptr] = *reg;
		(*hyp_memblock_nr_ptr)++;
	}

	return 0;
}

void __init kvm_hyp_reserve(void)
{
	u64 hyp_mem_pages = 0;
	int ret;

	if (!is_hyp_mode_available() || is_kernel_in_hyp_mode())
		return;

	if (kvm_get_mode() != KVM_MODE_PROTECTED)
		return;

	ret = register_memblock_regions();
	if (ret) {
		*hyp_memblock_nr_ptr = 0;
		kvm_err("Failed to register hyp memblocks: %d\n", ret);
		return;
	}

	hyp_mem_pages += hyp_s1_pgtable_pages();
	hyp_mem_pages += host_s2_pgtable_pages();
	hyp_mem_pages += hyp_vm_table_pages();
	hyp_mem_pages += hyp_vmemmap_pages(STRUCT_HYP_PAGE_SIZE);
	hyp_mem_pages += pkvm_selftest_pages();
	hyp_mem_pages += hyp_ffa_proxy_pages();
	hyp_mem_pages += kvm_iommu_pages();

	/*
	 * Try to allocate a PMD-aligned region to reduce TLB pressure once
	 * this is unmapped from the host stage-2, and fallback to PAGE_SIZE.
	 */
	hyp_mem_size = hyp_mem_pages << PAGE_SHIFT;
	hyp_mem_base = memblock_phys_alloc(ALIGN(hyp_mem_size, PMD_SIZE),
					   PMD_SIZE);
	if (!hyp_mem_base)
		hyp_mem_base = memblock_phys_alloc(hyp_mem_size, PAGE_SIZE);
	else
		hyp_mem_size = ALIGN(hyp_mem_size, PMD_SIZE);

	if (!hyp_mem_base) {
		kvm_err("Failed to reserve hyp memory\n");
		return;
	}

	kvm_info("Reserved %lld MiB at 0x%llx\n", hyp_mem_size >> 20,
		 hyp_mem_base);
}

static void __pkvm_destroy_hyp_vm(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu;
	unsigned long idx;

	if (pkvm_hyp_vm_is_created(kvm)) {
		WARN_ON(kvm_call_hyp_nvhe(__pkvm_finalize_teardown_vm,
					  kvm->arch.pkvm.handle));
	} else if (kvm->arch.pkvm.handle) {
		/*
		 * The VM could have been reserved but hyp initialization has
		 * failed. Make sure to unreserve it.
		 */
		kvm_call_hyp_nvhe(__pkvm_unreserve_vm, kvm->arch.pkvm.handle);
	}

	kvm->arch.pkvm.handle = 0;
	kvm->arch.pkvm.is_created = false;
	free_hyp_memcache(&kvm->arch.pkvm.teardown_mc);
	free_hyp_memcache(&kvm->arch.pkvm.stage2_teardown_mc);

	kvm_for_each_vcpu(idx, vcpu, kvm) {
		struct kvm_hyp_req *hyp_reqs = vcpu->arch.hyp_reqs;

		if (hyp_reqs) {
			kvm_unshare_hyp(hyp_reqs, hyp_reqs + 1);
			free_page((unsigned long)hyp_reqs);
			vcpu->arch.hyp_reqs = NULL;
		}
		kvm_iommu_guest_free_mc(&vcpu->arch.iommu_mc);
	}
}

static int __pkvm_create_hyp_vcpu(struct kvm_vcpu *vcpu)
{
	size_t hyp_vcpu_sz = PAGE_ALIGN(PKVM_HYP_VCPU_SIZE);
	pkvm_handle_t handle = vcpu->kvm->arch.pkvm.handle;
	struct kvm_hyp_req *hyp_reqs;
	void *hyp_vcpu;
	int ret;

	vcpu->arch.pkvm_memcache.flags |= HYP_MEMCACHE_ACCOUNT_STAGE2;

	hyp_vcpu = alloc_pages_exact(hyp_vcpu_sz, GFP_KERNEL_ACCOUNT);
	if (!hyp_vcpu)
		return -ENOMEM;

	hyp_reqs = (struct kvm_hyp_req *)__get_free_page(GFP_KERNEL_ACCOUNT);
	if (!hyp_reqs) {
		free_pages_exact(hyp_vcpu, hyp_vcpu_sz);
		return -ENOMEM;
	}

	ret = kvm_share_hyp(hyp_reqs, hyp_reqs + 1);
	if (ret)
		goto free_reqs;

	vcpu->arch.hyp_reqs = hyp_reqs;

	ret = kvm_call_hyp_nvhe(__pkvm_init_vcpu, handle, vcpu, hyp_vcpu);
	if (!ret)
		vcpu_set_flag(vcpu, VCPU_PKVM_FINALIZED);
	else {
		kvm_unshare_hyp(hyp_reqs, hyp_reqs + 1);
		vcpu->arch.hyp_reqs = NULL;
		free_reqs:
		free_page((unsigned long)hyp_reqs);
		free_pages_exact(hyp_vcpu, hyp_vcpu_sz);
	}

	return ret;
}

/*
 * Allocates and donates memory for hypervisor VM structs at EL2.
 *
 * Allocates space for the VM state, which includes the hyp vm as well as
 * the hyp vcpus.
 *
 * Stores an opaque handler in the kvm struct for future reference.
 *
 * Return 0 on success, negative error code on failure.
 */
static int __pkvm_create_hyp_vm(struct kvm *kvm)
{
	size_t pgd_sz, hyp_vm_sz;
	void *pgd, *hyp_vm;
	int ret;

	if (kvm->created_vcpus < 1)
		return -EINVAL;

	pgd_sz = kvm_pgtable_stage2_pgd_size(kvm->arch.mmu.vtcr);

	/*
	 * The PGD pages will be reclaimed using a hyp_memcache which implies
	 * page granularity. So, use alloc_pages_exact() to get individual
	 * refcounts.
	 */
	pgd = alloc_pages_exact(pgd_sz, GFP_KERNEL_ACCOUNT);
	if (!pgd)
		return -ENOMEM;

	/* Allocate memory to donate to hyp for vm and vcpu pointers. */
	hyp_vm_sz = PAGE_ALIGN(size_add(PKVM_HYP_VM_SIZE,
					size_mul(sizeof(void *),
						 kvm->created_vcpus)));
	hyp_vm = alloc_pages_exact(hyp_vm_sz, GFP_KERNEL_ACCOUNT);
	if (!hyp_vm) {
		ret = -ENOMEM;
		goto free_pgd;
	}

	/* Donate the VM memory to hyp and let hyp initialize it. */
	ret = kvm_call_hyp_nvhe(__pkvm_init_vm, kvm, hyp_vm, pgd);
	if (ret)
		goto free_vm;

	kvm->arch.pkvm.is_created = true;
	kvm->arch.pkvm.stage2_teardown_mc.flags |= HYP_MEMCACHE_ACCOUNT_STAGE2;
	kvm_account_pgtable_pages(pgd, pgd_sz / PAGE_SIZE);

	return 0;
free_vm:
	free_pages_exact(hyp_vm, hyp_vm_sz);
free_pgd:
	free_pages_exact(pgd, pgd_sz);
	return ret;
}

bool pkvm_hyp_vm_is_created(struct kvm *kvm)
{
	return READ_ONCE(kvm->arch.pkvm.is_created);
}

int pkvm_create_hyp_vm(struct kvm *kvm)
{
	int ret = 0;

	/*
	 * Synchronise with kvm_arch_prepare_memory_region(), as we
	 * prevent memslot modifications on a pVM that has been run.
	 */
	mutex_lock(&kvm->slots_lock);
	mutex_lock(&kvm->arch.config_lock);
	if (!pkvm_hyp_vm_is_created(kvm))
		ret = __pkvm_create_hyp_vm(kvm);
	mutex_unlock(&kvm->arch.config_lock);
	mutex_unlock(&kvm->slots_lock);

	return ret;
}

int pkvm_create_hyp_vcpu(struct kvm_vcpu *vcpu)
{
	int ret = 0;

	mutex_lock(&vcpu->kvm->arch.config_lock);
	if (!vcpu_get_flag(vcpu, VCPU_PKVM_FINALIZED))
		ret = __pkvm_create_hyp_vcpu(vcpu);
	mutex_unlock(&vcpu->kvm->arch.config_lock);

	return ret;
}

void pkvm_destroy_hyp_vm(struct kvm *kvm)
{
	mutex_lock(&kvm->arch.config_lock);
	__pkvm_destroy_hyp_vm(kvm);
	mutex_unlock(&kvm->arch.config_lock);
}

int pkvm_init_host_vm(struct kvm *kvm, unsigned long type)
{
	int ret;
	bool protected = type & KVM_VM_TYPE_ARM_PROTECTED;

	if (pkvm_hyp_vm_is_created(kvm))
		return -EINVAL;

	/* VM is already reserved, no need to proceed. */
	if (kvm->arch.pkvm.handle)
		return 0;

	/* Reserve the VM in hyp and obtain a hyp handle for the VM. */
	ret = kvm_call_hyp_nvhe(__pkvm_reserve_vm);
	if (ret < 0)
		return ret;

	kvm->arch.pkvm.handle = ret;
	kvm->arch.pkvm.is_protected = protected;
	if (protected) {
		pr_warn_once("kvm: protected VMs are experimental and for development only, tainting kernel\n");
		add_taint(TAINT_USER, LOCKDEP_STILL_OK);
	}

	return 0;
}

static int pkvm_register_device(struct of_phandle_args *args,
				struct pkvm_device *dev)
{
	struct device_node *np = args->np;
	struct platform_device *pdev;
	struct of_phandle_args iommu_spec;
	u32 group_id = args->args[0];
	struct resource res;
	pkvm_handle_t iommu_id;
	unsigned int idx = 0;
	int ret;

	while (!of_address_to_resource(np, idx, &res)) {
		if (idx >= PKVM_DEVICE_MAX_RESOURCE)
			return -E2BIG;
		if (!PAGE_ALIGNED(res.start) ||
		    !PAGE_ALIGNED(resource_size(&res)))
			return -EINVAL;

		dev->resources[idx].base = res.start;
		dev->resources[idx].size = resource_size(&res);
		idx++;
	}
	dev->nr_resources = idx;

	idx = 0;
	while (!of_parse_phandle_with_args(np, "iommus", "#iommu-cells",
					   idx, &iommu_spec)) {
		u64 endpoint;

		if (idx >= PKVM_DEVICE_MAX_IOMMU) {
			of_node_put(iommu_spec.np);
			return -E2BIG;
		}

		if (iommu_spec.args_count == 1)
			endpoint = iommu_spec.args[0];
		else if (kvm_get_iommu_endpoint(&iommu_spec, &endpoint)) {
			of_node_put(iommu_spec.np);
			return -EINVAL;
		}

		ret = kvm_get_iommu_id_by_of(iommu_spec.np, &iommu_id);
		if (ret) {
			of_node_put(iommu_spec.np);
			return ret;
		}

		dev->iommus[idx].id = iommu_id;
		dev->iommus[idx].endpoint = endpoint;
		of_node_put(iommu_spec.np);
		idx++;
	}

	dev->nr_iommus = idx;
	dev->group_id = group_id;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return -ENODEV;
	ret = kvm_iommu_prepare_protected_device(&pdev->dev);
	put_device(&pdev->dev);
	if (ret)
		return ret;

	return 0;
}

static int pkvm_register_pci_device(struct device_node *np,
				    struct pkvm_device *dev)
{
	struct pci_dev *pdev;
	pkvm_handle_t iommu_id;
	u32 domain, bus, devfn, group_id;
	u32 vendor_id, device_id;
	unsigned int idx, nr_iommus;
	int ret;

	ret = of_property_read_u32(np, "pci-domain", &domain);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "pci-bus", &bus);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "pci-devfn", &devfn);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "vendor-id", &vendor_id);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "device-id", &device_id);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "group-id", &group_id);
	if (ret)
		return ret;
	if (domain > INT_MAX || bus > U8_MAX || devfn > U8_MAX ||
	    vendor_id > U16_MAX || device_id > U16_MAX)
		return -ERANGE;

	pdev = pci_get_domain_bus_and_slot(domain, bus, devfn);
	if (!pdev) {
		pr_err("Protected PCI device %04x:%02x:%02x.%u is not enumerated\n",
		       domain, bus, PCI_SLOT(devfn), PCI_FUNC(devfn));
		return -ENODEV;
	}
	if (pdev->vendor != vendor_id || pdev->device != device_id) {
		pr_err("Protected PCI device %s has ID %04x:%04x, expected %04x:%04x\n",
		       pci_name(pdev), pdev->vendor, pdev->device,
		       vendor_id, device_id);
		ret = -ENODEV;
		goto out_put_device;
	}

	for (idx = 0; idx < PCI_STD_NUM_BARS; idx++) {
		resource_size_t start, size;

		if (!(pci_resource_flags(pdev, idx) & IORESOURCE_MEM))
			continue;
		start = pci_resource_start(pdev, idx);
		size = pci_resource_len(pdev, idx);
		if (!start || !size)
			continue;
		if (dev->nr_resources >= PKVM_DEVICE_MAX_RESOURCE) {
			ret = -E2BIG;
			goto out_put_device;
		}
		if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(size)) {
			ret = -EINVAL;
			goto out_put_device;
		}
		dev->resources[dev->nr_resources].base = start;
		dev->resources[dev->nr_resources].size = size;
		dev->nr_resources++;
	}
	if (!dev->nr_resources) {
		pr_err("Protected PCI device %s has no page-aligned memory BARs\n",
		       pci_name(pdev));
		ret = -ENODEV;
		goto out_put_device;
	}

	ret = kvm_iommu_device_num_ids(&pdev->dev);
	if (ret < 0) {
		pr_err("Failed to count IOMMU IDs for protected PCI device %s: %d\n",
		       pci_name(pdev), ret);
		goto out_put_device;
	}
	nr_iommus = ret;
	if (!nr_iommus) {
		pr_err("Protected PCI device %s has no pKVM IOMMU IDs\n",
		       pci_name(pdev));
		ret = -ENODEV;
		goto out_put_device;
	}
	if (nr_iommus > PKVM_DEVICE_MAX_IOMMU) {
		ret = -E2BIG;
		goto out_put_device;
	}
	for (idx = 0; idx < nr_iommus; idx++) {
		u32 endpoint;

		ret = kvm_iommu_device_id(&pdev->dev, idx, &iommu_id,
					  &endpoint);
		if (ret) {
			pr_err("Failed to resolve IOMMU ID %u for protected PCI device %s: %d\n",
			       idx, pci_name(pdev), ret);
			goto out_put_device;
		}
		dev->iommus[idx].id = iommu_id;
		dev->iommus[idx].endpoint = endpoint;
	}
	dev->nr_iommus = nr_iommus;
	dev->group_id = group_id;

	ret = kvm_iommu_prepare_protected_device(&pdev->dev);
	if (ret)
		pr_err("Failed to prepare protected PCI device %s: %d\n",
		       pci_name(pdev), ret);

out_put_device:
	pci_dev_put(pdev);
	return ret;
}

static int __init pkvm_probe_pci_device(struct device_node *assign_np)
{
	struct platform_device *pdev;
	struct pci_dev *pci_dev;
	struct device_node *np;
	ktime_t start;
	unsigned long deadline;
	u32 domain, bus, devfn, candidate;
	int ret;

	ret = of_property_read_u32(assign_np, "pci-domain", &domain);
	if (ret)
		return ret;
	ret = of_property_read_u32(assign_np, "pci-bus", &bus);
	if (ret)
		return ret;
	ret = of_property_read_u32(assign_np, "pci-devfn", &devfn);
	if (ret)
		return ret;
	if (domain > INT_MAX || bus > U8_MAX || devfn > U8_MAX)
		return -ERANGE;

	for_each_node_with_property(np, "linux,pci-domain") {
		if (of_property_read_u32(np, "linux,pci-domain", &candidate) ||
		    candidate != domain)
			continue;

		pdev = of_find_device_by_node(np);
		if (!pdev) {
			of_node_put(np);
			return -ENODEV;
		}

		/*
		 * PCI host drivers may prefer asynchronous probing. Request a
		 * synchronous attach, but do not depend on its return value: driver
		 * probe failures are intentionally hidden by device_attach(), and an
		 * already queued asynchronous probe may complete instead. Wait only
		 * for the protected endpoint to be enumerated, without draining
		 * unrelated probes and allowing arbitrary DMA before host protection
		 * is final. The endpoint driver may be modular and is not required for
		 * pKVM to record the device's BARs and IOMMU identity.
		 */
		ret = device_attach(&pdev->dev);
		put_device(&pdev->dev);
		of_node_put(np);
		if (ret < 0 && ret != -EPROBE_DEFER)
			return ret;

		start = ktime_get();
		deadline = jiffies + msecs_to_jiffies(PKVM_PCI_PROBE_TIMEOUT_MS);
		do {
			pci_dev = pci_get_domain_bus_and_slot(domain, bus, devfn);
			if (pci_dev) {
				pci_dev_put(pci_dev);
				pr_info("Protected PCI device %04x:%02x:%02x.%u enumerated after %lld us\n",
					domain, bus, PCI_SLOT(devfn),
					PCI_FUNC(devfn),
					ktime_us_delta(ktime_get(), start));
				return 0;
			}
			msleep(PKVM_PCI_PROBE_INTERVAL_MS);
		} while (time_before(jiffies, deadline));
		pr_err("Protected PCI device %04x:%02x:%02x.%u was not enumerated after %lld us\n",
		       domain, bus, PCI_SLOT(devfn), PCI_FUNC(devfn),
		       ktime_us_delta(ktime_get(), start));
		return -EPROBE_DEFER;
	}

	return -ENODEV;
}

static int __init pkvm_probe_protected_pci_hosts(void)
{
	struct device_node *np;
	int ret;

	for_each_compatible_node(np, NULL, PKVM_PCI_DEVICE_ASSIGN_COMPAT) {
		ret = pkvm_probe_pci_device(np);
		if (ret) {
			pr_err("Failed to probe protected PCI device: %d\n", ret);
			goto out_put_node;
		}
	}

	return 0;

out_put_node:
	of_node_put(np);
	return ret;
}

static int pkvm_register_protected_devices(void)
{
	struct pkvm_device *devices;
	struct device_node *np;
	size_t devices_size;
	int count = 0, idx = 0, ret;

	for_each_compatible_node(np, NULL, PKVM_DEVICE_ASSIGN_COMPAT) {
		struct of_phandle_args args;
		int entry = 0;

		while (!of_parse_phandle_with_fixed_args(np, "devices", 1,
						 entry++, &args)) {
			count++;
			of_node_put(args.np);
		}
	}
	for_each_compatible_node(np, NULL, PKVM_PCI_DEVICE_ASSIGN_COMPAT)
		count++;

	if (!count)
		return 0;

	devices_size = PAGE_ALIGN(size_mul(sizeof(*devices), count));
	devices = alloc_pages_exact(devices_size, GFP_KERNEL_ACCOUNT);
	if (!devices)
		return -ENOMEM;
	memset(devices, 0, devices_size);

	for_each_compatible_node(np, NULL, PKVM_DEVICE_ASSIGN_COMPAT) {
		struct of_phandle_args args;
		int entry = 0;

		while (!of_parse_phandle_with_fixed_args(np, "devices", 1,
						 entry++, &args)) {
			ret = pkvm_register_device(&args, &devices[idx++]);
			of_node_put(args.np);
			if (ret) {
				of_node_put(np);
				goto free_devices;
			}
		}
	}
	for_each_compatible_node(np, NULL, PKVM_PCI_DEVICE_ASSIGN_COMPAT) {
		ret = pkvm_register_pci_device(np, &devices[idx++]);
		if (ret) {
			of_node_put(np);
			goto free_devices;
		}
	}

	kvm_nvhe_sym(registered_devices_nr) = count;
	kvm_nvhe_sym(registered_devices) = devices;
	ret = kvm_call_hyp_nvhe(__pkvm_devices_init);
	if (!ret)
		return 0;

free_devices:
	free_pages_exact(devices, devices_size);
	kvm_nvhe_sym(registered_devices_nr) = 0;
	kvm_nvhe_sym(registered_devices) = NULL;
	return ret;
}

static void __init _kvm_host_prot_finalize(void *arg)
{
	int *err = arg;

	if (WARN_ON(kvm_call_hyp_nvhe(__pkvm_prot_finalize)))
		WRITE_ONCE(*err, -EINVAL);
}

static int __init pkvm_drop_host_privileges(void)
{
	int ret = 0;

	/*
	 * Flip the static key upfront as that may no longer be possible
	 * once the host stage 2 is installed.
	 */
	static_branch_enable(&kvm_protected_mode_initialized);
	on_each_cpu(_kvm_host_prot_finalize, &ret, 1);
	return ret;
}

static int __init finalize_pkvm(void)
{
	int ret;

	if (!is_protected_kvm_enabled() || !is_kvm_arm_initialised())
		return 0;

	ret = kvm_iommu_init_driver();
	if (ret && ret != -ENODEV)
		return ret;

	ret = pkvm_probe_protected_pci_hosts();
	if (ret)
		return ret;

	ret = pkvm_register_protected_devices();
	if (ret) {
		pr_err("Failed to initialize protected devices: %d\n", ret);
		return ret;
	}

	/*
	 * Exclude HYP sections from kmemleak so that they don't get peeked
	 * at, which would end badly once inaccessible.
	 */
	kmemleak_free_part(__hyp_bss_start, __hyp_bss_end - __hyp_bss_start);
	kmemleak_free_part(__hyp_data_start, __hyp_data_end - __hyp_data_start);
	kmemleak_free_part(__hyp_rodata_start, __hyp_rodata_end - __hyp_rodata_start);
	kmemleak_free_part_phys(hyp_mem_base, hyp_mem_size);

	ret = pkvm_drop_host_privileges();
	if (ret)
		pr_err("Failed to finalize Hyp protection: %d\n", ret);

	return ret;
}

/*
 * Protected PCI hosts can remain deferred until their late-init suppliers
 * have probed.  Run after deferred_probe_initcall() has flushed those probes,
 * but still before init memory is freed and userspace starts.
 */
late_initcall_sync(finalize_pkvm);

static u64 __pkvm_mapping_start(struct pkvm_mapping *m)
{
	return m->gfn * PAGE_SIZE;
}

static u64 __pkvm_mapping_end(struct pkvm_mapping *m)
{
	return (m->gfn + m->nr_pages) * PAGE_SIZE - 1;
}

INTERVAL_TREE_DEFINE(struct pkvm_mapping, node, u64, __subtree_last,
		     __pkvm_mapping_start, __pkvm_mapping_end, static,
		     pkvm_mapping);

/*
 * __tmp is updated to iter_first(pkvm_mappings) *before* entering the body of the loop to allow
 * freeing of __map inline.
 */
#define for_each_mapping_in_range_safe(__pgt, __start, __end, __map)				\
	for (struct pkvm_mapping *__tmp = pkvm_mapping_iter_first(&(__pgt)->pkvm_mappings,	\
								  __start, __end - 1);		\
	     __tmp && ({									\
				__map = __tmp;							\
				__tmp = pkvm_mapping_iter_next(__map, __start, __end - 1);	\
				true;								\
		       });									\
	    )

int pkvm_pgtable_stage2_init(struct kvm_pgtable *pgt, struct kvm_s2_mmu *mmu,
			     struct kvm_pgtable_mm_ops *mm_ops)
{
	pgt->pkvm_mappings	= RB_ROOT_CACHED;
	pgt->mmu		= mmu;

	return 0;
}

static int __pkvm_pgtable_stage2_reclaim(struct kvm_pgtable *pgt, u64 start, u64 end)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct pkvm_mapping *mapping;
	int ret;

	for_each_mapping_in_range_safe(pgt, start, end, mapping) {
		struct page *page;

		ret = kvm_call_hyp_nvhe(__pkvm_reclaim_dying_guest_page,
					handle, mapping->gfn);
		if (WARN_ON(ret))
			continue;

		page = pfn_to_page(mapping->pfn);
		WARN_ON_ONCE(mapping->nr_pages != 1);
		unpin_user_pages_dirty_lock(&page, 1, true);
		account_locked_vm(kvm->mm, 1, false);
		pkvm_mapping_remove(mapping, &pgt->pkvm_mappings);
		kfree(mapping);
	}

	return 0;
}

static int __pkvm_pgtable_stage2_unshare(struct kvm_pgtable *pgt, u64 start, u64 end)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct pkvm_mapping *mapping;
	int ret;

	for_each_mapping_in_range_safe(pgt, start, end, mapping) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_guest, handle, mapping->gfn,
					mapping->nr_pages);
		if (WARN_ON(ret))
			return ret;
		pkvm_mapping_remove(mapping, &pgt->pkvm_mappings);
		kfree(mapping);
	}

	return 0;
}

void pkvm_pgtable_stage2_destroy_range(struct kvm_pgtable *pgt,
					u64 addr, u64 size)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;

	if (!handle)
		return;

	if (pkvm_hyp_vm_is_created(kvm) && !kvm->arch.pkvm.is_dying) {
		WARN_ON(kvm_call_hyp_nvhe(__pkvm_start_teardown_vm, handle));
		kvm->arch.pkvm.is_dying = true;
	}

	if (kvm_vm_is_protected(kvm))
		__pkvm_pgtable_stage2_reclaim(pgt, addr, addr + size);
	else
		__pkvm_pgtable_stage2_unshare(pgt, addr, addr + size);
}

void pkvm_pgtable_stage2_destroy_pgd(struct kvm_pgtable *pgt)
{
	/* Expected to be called after all pKVM mappings have been released. */
	WARN_ON_ONCE(!RB_EMPTY_ROOT(&pgt->pkvm_mappings.rb_root));
}

int pkvm_pgtable_stage2_map(struct kvm_pgtable *pgt, u64 addr, u64 size,
			   u64 phys, enum kvm_pgtable_prot prot,
			   void *mc, enum kvm_pgtable_walk_flags flags)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	struct pkvm_mapping *mapping = NULL;
	struct kvm_hyp_memcache *cache = mc;
	u64 gfn = addr >> PAGE_SHIFT;
	u64 pfn = phys >> PAGE_SHIFT;
	u64 end = addr + size;
	int ret;

	lockdep_assert_held_write(&kvm->mmu_lock);
	mapping = pkvm_mapping_iter_first(&pgt->pkvm_mappings, addr, end - 1);

	if (kvm_vm_is_protected(kvm)) {
		/* Protected VMs are mapped using RWX page-granular mappings */
		if (WARN_ON_ONCE(size != PAGE_SIZE))
			return -EINVAL;

		if (WARN_ON_ONCE(prot != KVM_PGTABLE_PROT_RWX))
			return -EINVAL;

		/*
		 * We either raced with another vCPU or the guest PTE
		 * has been poisoned by an erroneous host access.
		 */
		if (mapping) {
			ret = kvm_call_hyp_nvhe(__pkvm_vcpu_in_poison_fault);
			return ret ? -EFAULT : -EAGAIN;
		}

		ret = kvm_call_hyp_nvhe(__pkvm_host_donate_guest, pfn, gfn);
	} else {
		if (WARN_ON_ONCE(size != PAGE_SIZE && size != PMD_SIZE))
			return -EINVAL;

		/*
		 * We either raced with another vCPU or we're changing between
		 * page and block mappings. As per user_mem_abort(), same-size
		 * permission faults are handled in the relax_perms() path.
		 */
		if (mapping) {
			if (size == (mapping->nr_pages * PAGE_SIZE))
				return -EAGAIN;

			/*
			 * Remove _any_ pkvm_mapping overlapping with the range,
			 * bigger or smaller.
			 */
			ret = __pkvm_pgtable_stage2_unshare(pgt, addr, end);
			if (ret)
				return ret;

			mapping = NULL;
		}

		ret = kvm_call_hyp_nvhe(__pkvm_host_share_guest, pfn, gfn,
					size / PAGE_SIZE, prot);
	}

	if (WARN_ON(ret))
		return ret;

	swap(mapping, cache->mapping);
	mapping->gfn = gfn;
	mapping->pfn = pfn;
	mapping->nr_pages = size / PAGE_SIZE;
	pkvm_mapping_insert(mapping, &pgt->pkvm_mappings);

	return ret;
}

int pkvm_pgtable_stage2_unmap(struct kvm_pgtable *pgt, u64 addr, u64 size)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);

	if (WARN_ON(kvm_vm_is_protected(kvm)))
		return -EPERM;

	lockdep_assert_held_write(&kvm->mmu_lock);

	return __pkvm_pgtable_stage2_unshare(pgt, addr, addr + size);
}

int pkvm_pgtable_stage2_wrprotect(struct kvm_pgtable *pgt, u64 addr, u64 size)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct pkvm_mapping *mapping;
	int ret = 0;

	if (WARN_ON(kvm_vm_is_protected(kvm)))
		return -EPERM;

	lockdep_assert_held(&kvm->mmu_lock);
	for_each_mapping_in_range_safe(pgt, addr, addr + size, mapping) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_wrprotect_guest, handle, mapping->gfn,
					mapping->nr_pages);
		if (WARN_ON(ret))
			break;
	}

	return ret;
}

int pkvm_pgtable_stage2_flush(struct kvm_pgtable *pgt, u64 addr, u64 size)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	struct pkvm_mapping *mapping;

	lockdep_assert_held(&kvm->mmu_lock);
	for_each_mapping_in_range_safe(pgt, addr, addr + size, mapping)
		__clean_dcache_guest_page(pfn_to_kaddr(mapping->pfn),
					  PAGE_SIZE * mapping->nr_pages);

	return 0;
}

bool pkvm_pgtable_stage2_test_clear_young(struct kvm_pgtable *pgt, u64 addr, u64 size, bool mkold)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(pgt->mmu);
	pkvm_handle_t handle = kvm->arch.pkvm.handle;
	struct pkvm_mapping *mapping;
	bool young = false;

	if (WARN_ON(kvm_vm_is_protected(kvm)))
		return false;

	lockdep_assert_held(&kvm->mmu_lock);
	for_each_mapping_in_range_safe(pgt, addr, addr + size, mapping)
		young |= kvm_call_hyp_nvhe(__pkvm_host_test_clear_young_guest, handle, mapping->gfn,
					   mapping->nr_pages, mkold);

	return young;
}

int pkvm_pgtable_stage2_relax_perms(struct kvm_pgtable *pgt, u64 addr, enum kvm_pgtable_prot prot,
				    enum kvm_pgtable_walk_flags flags)
{
	if (WARN_ON(kvm_vm_is_protected(kvm_s2_mmu_to_kvm(pgt->mmu))))
		return -EPERM;

	return kvm_call_hyp_nvhe(__pkvm_host_relax_perms_guest, addr >> PAGE_SHIFT, prot);
}

void pkvm_pgtable_stage2_mkyoung(struct kvm_pgtable *pgt, u64 addr,
				 enum kvm_pgtable_walk_flags flags)
{
	if (WARN_ON(kvm_vm_is_protected(kvm_s2_mmu_to_kvm(pgt->mmu))))
		return;

	WARN_ON(kvm_call_hyp_nvhe(__pkvm_host_mkyoung_guest, addr >> PAGE_SHIFT));
}

int __pkvm_topup_hyp_alloc_mgt_mc(enum hyp_alloc_mgt_id id,
				  struct kvm_hyp_memcache *mc)
{
	struct arm_smccc_res res;
	int ret;

	do {
		res = kvm_call_hyp_nvhe_smccc(__pkvm_hyp_alloc_mgt_refill,
					      id, mc->head, mc->nr_pages);
		ret = res.a1;
		mc->head = res.a3 & PAGE_MASK;
		mc->nr_pages = res.a3 & ~PAGE_MASK;

		if (!ret)
			break;

		ret = __pkvm_handle_smccc_req(&res, NULL);
		if (ret)
			return ret;
	} while (1);

	return 0;
}

int __pkvm_topup_hyp_alloc(unsigned long nr_pages)
{
	struct kvm_hyp_memcache mc;
	int ret;

	init_hyp_memcache(&mc);
	ret = topup_hyp_memcache(&mc, nr_pages, 0);
	if (ret)
		return ret;

	ret = __pkvm_topup_hyp_alloc_mgt_mc(HYP_ALLOC_MGT_HEAP_ID, &mc);
	if (ret)
		free_hyp_memcache(&mc);

	return ret;
}
EXPORT_SYMBOL(__pkvm_topup_hyp_alloc);

int handle_hyp_req(struct kvm_vcpu *vcpu, struct kvm_hyp_req *req, void *arg)
{
	switch (req->type) {
	case KVM_HYP_REQ_TYPE_HYP_ALLOC:
		return __pkvm_topup_hyp_alloc(req->mem.nr_pages);
	case KVM_HYP_REQ_TYPE_MEM_IOMMU:
		return __pkvm_topup_hyp_iommu(1,
				req->mem.nr_pages << PAGE_SHIFT,
				arg ? (gfp_t)(uintptr_t)arg : GFP_KERNEL);
	case KVM_HYP_REQ_TYPE_MEM:
		if (!vcpu)
			return -EINVAL;
		if (req->memcache.dest == REQ_MEM_DEST_VCPU_MEMCACHE)
			return topup_hyp_memcache(&vcpu->arch.pkvm_memcache,
						    req->memcache.nr_pages, 0);
		if (req->memcache.dest == REQ_MEM_DEST_HYP_IOMMU)
			return kvm_iommu_guest_alloc_mc(&vcpu->arch.iommu_mc,
							req->memcache.sz_alloc,
							req->memcache.nr_pages);
		return -EINVAL;
	case KVM_HYP_REQ_TYPE_MAP:
		if (!vcpu)
			return -EINVAL;
		return pkvm_mem_abort_range(vcpu, req->map.guest_ipa,
					    req->map.size);
	case KVM_HYP_REQ_TYPE_SPLIT:
		return -EOPNOTSUPP;
	case KVM_HYP_LAST_REQ:
		return 0;
	default:
		return -EINVAL;
	}
}

int __pkvm_handle_smccc_req(struct arm_smccc_res *res, void *arg)
{
	struct kvm_hyp_req req;

	if (smccc_to_hyp_req(&req, res))
		return handle_hyp_req(NULL, &req, arg);

	return res->a1;
}

static int pkvm_donate_resource(struct resource *resource)
{
	if (!PAGE_ALIGNED(resource->start) ||
	    !PAGE_ALIGNED(resource_size(resource)))
		return -EINVAL;

	return kvm_call_hyp_nvhe(__pkvm_host_donate_hyp_mmio,
				 __phys_to_pfn(resource->start),
				 resource_size(resource) >> PAGE_SHIFT);
}

static int pkvm_reclaim_resource(struct resource *resource)
{
	if (!PAGE_ALIGNED(resource->start) ||
	    !PAGE_ALIGNED(resource_size(resource)))
		return -EINVAL;

	return kvm_call_hyp_nvhe(__pkvm_host_reclaim_hyp_mmio,
				 __phys_to_pfn(resource->start),
				 resource_size(resource) >> PAGE_SHIFT);
}

static int pkvm_assign_platform_device(struct device *dev, void *data)
{
	struct platform_device *pdev;
	struct resource *resource;
	int index = 0;
	int ret;

	if (!dev_is_platform(dev))
		return -EOPNOTSUPP;

	pdev = to_platform_device(dev);
	while ((resource = platform_get_resource(pdev, IORESOURCE_MEM, index))) {
		ret = pkvm_donate_resource(resource);
		if (ret)
			goto err_reclaim;
		index++;
	}

	return 0;

err_reclaim:
	while (index--)
		pkvm_reclaim_resource(platform_get_resource(pdev, IORESOURCE_MEM,
						    index));

	return ret;
}

static int pkvm_reclaim_platform_device(struct device *dev, void *data)
{
	struct platform_device *pdev;
	struct resource *resource;
	int index = 0;

	if (!dev_is_platform(dev))
		return -EOPNOTSUPP;

	pdev = to_platform_device(dev);
	while ((resource = platform_get_resource(pdev, IORESOURCE_MEM, index++)))
		pkvm_reclaim_resource(resource);

	return 0;
}

int kvm_arch_assign_device(struct device *dev)
{
	if (!is_protected_kvm_enabled())
		return 0;

	return pkvm_assign_platform_device(dev, NULL);
}

int kvm_arch_assign_group(struct iommu_group *group)
{
	int ret;

	if (!is_protected_kvm_enabled())
		return 0;

	ret = iommu_group_for_each_dev(group, NULL,
				       pkvm_assign_platform_device);
	if (ret)
		iommu_group_for_each_dev(group, NULL,
					 pkvm_reclaim_platform_device);

	return ret;
}

void kvm_arch_reclaim_device(struct device *dev)
{
	if (is_protected_kvm_enabled())
		pkvm_reclaim_platform_device(dev, NULL);
}

void kvm_arch_reclaim_group(struct iommu_group *group)
{
	if (is_protected_kvm_enabled())
		iommu_group_for_each_dev(group, NULL,
					 pkvm_reclaim_platform_device);
}

void pkvm_pgtable_stage2_free_unlinked(struct kvm_pgtable_mm_ops *mm_ops, void *pgtable, s8 level)
{
	WARN_ON_ONCE(1);
}

kvm_pte_t *pkvm_pgtable_stage2_create_unlinked(struct kvm_pgtable *pgt, u64 phys, s8 level,
					enum kvm_pgtable_prot prot, void *mc, bool force_pte)
{
	WARN_ON_ONCE(1);
	return NULL;
}

int pkvm_pgtable_stage2_split(struct kvm_pgtable *pgt, u64 addr, u64 size,
			      struct kvm_mmu_memory_cache *mc)
{
	WARN_ON_ONCE(1);
	return -EINVAL;
}

/*
 * Forcefully reclaim a page from the guest, zeroing its contents and
 * poisoning the stage-2 pte so that pages can no longer be mapped at
 * the same IPA. The page remains pinned until the guest is destroyed.
 */
bool pkvm_force_reclaim_guest_page(phys_addr_t phys)
{
	int ret = kvm_call_hyp_nvhe(__pkvm_force_reclaim_guest_page, phys);

	return !ret || ret == -EAGAIN;
}
