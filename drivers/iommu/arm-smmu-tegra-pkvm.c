// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host frontend for the Tegra234 SMMUv2 pKVM backend.
 *
 * Copyright (C) 2026 TII (SSRC) and the Ghaf contributors
 */

#include <asm/kvm_host.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/bitfield.h>
#include <linux/idr.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include <soc/tegra/mc.h>

#include <kvm/tegra-smmu-pkvm.h>

DECLARE_KVM_NVHE_SYM(pkvm_tegra_smmu_count);
DECLARE_KVM_NVHE_SYM(pkvm_tegra_smmu_devices);
DECLARE_KVM_NVHE_SYM(pkvm_tegra_smmu_ops);

struct pkvm_tegra_host_smmu {
	struct iommu_device iommu;
	struct device *dev;
	struct tegra_mc *mc;
	struct mutex group_lock;
	struct list_head groups;
	pkvm_handle_t id;
	u32 address_bits;
};

struct pkvm_tegra_group {
	struct list_head list;
	struct pkvm_tegra_host_smmu *smmu;
	struct iommu_group *group;
	u32 *sids;
	unsigned int num_sids;
};

struct pkvm_tegra_master {
	struct device *dev;
	struct pkvm_tegra_host_smmu *smmu;
};

struct pkvm_tegra_domain {
	struct iommu_domain domain;
	struct pkvm_tegra_host_smmu *smmu;
	pkvm_handle_t id;
	unsigned int debug_maps;
	struct delayed_work debug_work;
	bool debug_work_scheduled;
};

#define to_pkvm_tegra_domain(d) \
	container_of(d, struct pkvm_tegra_domain, domain)

static DEFINE_IDA(pkvm_tegra_domain_ids);
static struct platform_driver pkvm_tegra_smmu_driver;
static struct pkvm_tegra_smmu_device *pkvm_tegra_smmus;
static size_t pkvm_tegra_smmu_count;
static size_t pkvm_tegra_smmu_current;
static pkvm_handle_t pkvm_tegra_hyp_driver;

static void pkvm_tegra_debug_workfn(struct work_struct *work)
{
	struct pkvm_tegra_domain *domain = container_of(
		to_delayed_work(work), struct pkvm_tegra_domain, debug_work);
	u64 value[7][2] = { };
	int ret[7];
	int op;

	for (op = 0; op < ARRAY_SIZE(ret); op++)
		ret[op] = kvm_iommu_debug_read(
			pkvm_tegra_hyp_driver, domain->smmu->id,
			PKVM_TEGRA_DEBUG_DOMAIN_SEL(domain->id, op),
			&value[op][0], &value[op][1]);
	for (op = 0; op < ARRAY_SIZE(ret); op++)
		pr_err("tegra-pkvm-post-dma: smmu=%llu domain=%llu op=%d ret=%d value0=%#llx value1=%#llx\n",
		       (unsigned long long)domain->smmu->id,
		       (unsigned long long)domain->id, op, ret[op],
		       (unsigned long long)value[op][0],
		       (unsigned long long)value[op][1]);
}

static unsigned int pkvm_tegra_id_size(u32 value)
{
	static const u8 sizes[] = { 32, 36, 40, 42, 44, 48 };

	return value < ARRAY_SIZE(sizes) ? sizes[value] : 48;
}

static struct pkvm_tegra_host_smmu *
pkvm_tegra_smmu_from_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev;
	struct pkvm_tegra_host_smmu *smmu;

	dev = driver_find_device_by_fwnode(&pkvm_tegra_smmu_driver.driver,
					   fwnode);
	if (!dev)
		return NULL;
	smmu = dev_get_drvdata(dev);
	put_device(dev);
	return smmu;
}

static struct iommu_device *pkvm_tegra_probe_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pkvm_tegra_master *master;
	struct pkvm_tegra_host_smmu *smmu;

	if (!fwspec || dev_iommu_priv_get(dev))
		return ERR_PTR(-ENODEV);

	smmu = pkvm_tegra_smmu_from_fwnode(fwspec->iommu_fwnode);
	if (!smmu)
		return ERR_PTR(-ENODEV);

	master = kzalloc_obj(*master, GFP_KERNEL);
	if (!master)
		return ERR_PTR(-ENOMEM);

	master->dev = dev;
	master->smmu = smmu;
	dev_iommu_priv_set(dev, master);
	return &smmu->iommu;
}

static void pkvm_tegra_detach(struct device *dev, struct iommu_domain *domain)
{
	struct pkvm_tegra_master *master = dev_iommu_priv_get(dev);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pkvm_tegra_domain *tegra_domain;
	unsigned int i;

	if (!master || !fwspec || !domain ||
	    domain->type == IOMMU_DOMAIN_BLOCKED)
		return;

	if (domain->type == IOMMU_DOMAIN_IDENTITY) {
		for (i = 0; i < fwspec->num_ids; i++)
			WARN_ON(kvm_iommu_set_identity(pkvm_tegra_hyp_driver,
						       master->smmu->id,
						       fwspec->ids[i], false, 0));
		return;
	}

	tegra_domain = to_pkvm_tegra_domain(domain);
	for (i = 0; i < fwspec->num_ids; i++)
		WARN_ON(kvm_iommu_detach_dev(master->smmu->id,
					     tegra_domain->id,
					     fwspec->ids[i], 0));
}

static void pkvm_tegra_release_device(struct device *dev)
{
	struct pkvm_tegra_master *master = dev_iommu_priv_get(dev);
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	if (!master)
		return;
	pkvm_tegra_detach(dev, domain);
	dev_iommu_priv_set(dev, NULL);
	kfree(master);
}

static void pkvm_tegra_probe_finalize(struct device *dev)
{
	struct pkvm_tegra_master *master = dev_iommu_priv_get(dev);
	int ret;

	if (!master)
		return;
	ret = tegra_mc_probe_device(master->smmu->mc, dev);
	if (ret)
		dev_err(dev, "failed to program Tegra stream ID: %d\n", ret);
}

static int pkvm_tegra_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	if (args->args_count != 1)
		return -EINVAL;
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

static int pkvm_tegra_attach(struct iommu_domain *domain, struct device *dev,
			     struct iommu_domain *old)
{
	struct pkvm_tegra_master *master = dev_iommu_priv_get(dev);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pkvm_tegra_domain *tegra_domain;
	unsigned int i;
	int ret;

	if (!master || !fwspec)
		return -ENODEV;
	if (old && old != domain)
		pkvm_tegra_detach(dev, old);
	if (domain->type == IOMMU_DOMAIN_BLOCKED)
		return 0;

	if (domain->type == IOMMU_DOMAIN_IDENTITY) {
		for (i = 0; i < fwspec->num_ids; i++) {
			ret = kvm_iommu_set_identity(pkvm_tegra_hyp_driver,
						     master->smmu->id,
						     fwspec->ids[i], true, 0);
			if (ret) {
				dev_err(dev,
					"failed to attach identity domain to SMMU %llu SID %#x: %d\n",
					master->smmu->id, fwspec->ids[i], ret);
				goto err_detach;
			}
		}
		return 0;
	}

	tegra_domain = to_pkvm_tegra_domain(domain);
	if (tegra_domain->smmu != master->smmu)
		return -EINVAL;
	for (i = 0; i < fwspec->num_ids; i++) {
		ret = kvm_iommu_attach_dev(master->smmu->id, tegra_domain->id,
					   fwspec->ids[i], 0, 0, 0);
		if (ret)
			goto err_detach;
	}
	return 0;

err_detach:
	while (i--) {
		if (domain->type == IOMMU_DOMAIN_IDENTITY)
			kvm_iommu_set_identity(pkvm_tegra_hyp_driver,
					       master->smmu->id,
					       fwspec->ids[i], false, 0);
		else
			kvm_iommu_detach_dev(master->smmu->id, tegra_domain->id,
					     fwspec->ids[i], 0);
	}
	return ret;
}

static const struct iommu_domain_ops pkvm_tegra_identity_ops = {
	.attach_dev = pkvm_tegra_attach,
};

static struct iommu_domain pkvm_tegra_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &pkvm_tegra_identity_ops,
};

static const struct iommu_domain_ops pkvm_tegra_blocked_ops = {
	.attach_dev = pkvm_tegra_attach,
};

static struct iommu_domain pkvm_tegra_blocked_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &pkvm_tegra_blocked_ops,
};

static int pkvm_tegra_map_pages(struct iommu_domain *domain,
				unsigned long iova, phys_addr_t paddr,
				size_t pgsize, size_t pgcount, int prot,
				gfp_t gfp, size_t *mapped)
{
	struct pkvm_tegra_domain *tegra_domain = to_pkvm_tegra_domain(domain);
	u64 value[7][2] = { };
	int debug_ret[7];
	bool debug;
	int ret;
	int op;

	debug = tegra_domain->debug_maps++ < 32;
	if (debug)
		pr_err("tegra-pkvm-map-begin: smmu=%llu domain=%llu iova=%#lx pa=%pa pgsize=%zu pgcount=%zu prot=%#x\n",
		       (unsigned long long)tegra_domain->smmu->id,
		       (unsigned long long)tegra_domain->id, iova, &paddr,
		       pgsize, pgcount, prot);
	ret = kvm_iommu_map_pages(tegra_domain->id, iova, paddr, pgsize,
				  pgcount, prot, gfp, mapped);
	if (*mapped && !tegra_domain->debug_work_scheduled) {
		tegra_domain->debug_work_scheduled = true;
		schedule_delayed_work(&tegra_domain->debug_work,
				      msecs_to_jiffies(1000));
	}
	if (!debug)
		return ret;

	for (op = 0; op < ARRAY_SIZE(debug_ret); op++)
		debug_ret[op] = kvm_iommu_debug_read(
			pkvm_tegra_hyp_driver, tegra_domain->smmu->id,
			PKVM_TEGRA_DEBUG_DOMAIN_SEL(tegra_domain->id, op),
			&value[op][0], &value[op][1]);
	pr_err("tegra-pkvm-map: smmu=%llu domain=%llu iova=%#lx pa=%pa pgsize=%zu pgcount=%zu prot=%#x ret=%d mapped=%zu\n",
	       (unsigned long long)tegra_domain->smmu->id,
	       (unsigned long long)tegra_domain->id, iova, &paddr,
	       pgsize, pgcount, prot, ret, *mapped);
	for (op = 0; op < ARRAY_SIZE(debug_ret); op++)
		pr_err("tegra-pkvm-map: domain=%llu op=%d ret=%d value0=%#llx value1=%#llx\n",
		       (unsigned long long)tegra_domain->id, op, debug_ret[op],
		       (unsigned long long)value[op][0],
		       (unsigned long long)value[op][1]);
	return ret;
}

static size_t pkvm_tegra_unmap_pages(struct iommu_domain *domain,
				     unsigned long iova, size_t pgsize,
				     size_t pgcount,
				     struct iommu_iotlb_gather *gather)
{
	struct pkvm_tegra_domain *tegra_domain = to_pkvm_tegra_domain(domain);

	return kvm_iommu_unmap_pages(tegra_domain->id, iova, pgsize, pgcount);
}

static phys_addr_t pkvm_tegra_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	return kvm_iommu_iova_to_phys(to_pkvm_tegra_domain(domain)->id, iova);
}

static void pkvm_tegra_domain_free(struct iommu_domain *domain)
{
	struct pkvm_tegra_domain *tegra_domain = to_pkvm_tegra_domain(domain);

	cancel_delayed_work_sync(&tegra_domain->debug_work);
	WARN_ON(kvm_iommu_free_domain(tegra_domain->id));
	ida_free(&pkvm_tegra_domain_ids, tegra_domain->id);
	kfree(tegra_domain);
}

static const struct iommu_domain_ops pkvm_tegra_paging_ops = {
	.attach_dev = pkvm_tegra_attach,
	.map_pages = pkvm_tegra_map_pages,
	.unmap_pages = pkvm_tegra_unmap_pages,
	.iova_to_phys = pkvm_tegra_iova_to_phys,
	.free = pkvm_tegra_domain_free,
};

static struct iommu_domain *pkvm_tegra_domain_alloc_paging(struct device *dev)
{
	struct pkvm_tegra_master *master = dev_iommu_priv_get(dev);
	struct pkvm_tegra_domain *domain;
	int ret;

	if (!master)
		return ERR_PTR(-ENODEV);
	domain = kzalloc_obj(*domain, GFP_KERNEL);
	if (!domain)
		return ERR_PTR(-ENOMEM);

	ret = ida_alloc_range(&pkvm_tegra_domain_ids, 1,
			      KVM_IOMMU_MAX_HOST_DOMAINS - 1, GFP_KERNEL);
	if (ret < 0)
		goto err_free;
	domain->id = ret;
	domain->smmu = master->smmu;
	INIT_DELAYED_WORK(&domain->debug_work, pkvm_tegra_debug_workfn);
	domain->domain.ops = &pkvm_tegra_paging_ops;
	domain->domain.pgsize_bitmap = PAGE_SIZE;
	domain->domain.geometry.aperture_end =
		BIT_ULL(master->smmu->address_bits) - 1;
	domain->domain.geometry.force_aperture = true;

	ret = kvm_iommu_alloc_domain(pkvm_tegra_hyp_driver, master->smmu->id,
				     domain->id, 1);
	if (ret)
		goto err_id;
	return &domain->domain;

err_id:
	ida_free(&pkvm_tegra_domain_ids, domain->id);
err_free:
	kfree(domain);
	return ERR_PTR(ret);
}

static bool pkvm_tegra_group_has_sid(const struct pkvm_tegra_group *group,
				     u32 sid)
{
	unsigned int i;

	for (i = 0; i < group->num_sids; i++)
		if (group->sids[i] == sid)
			return true;
	return false;
}

static void pkvm_tegra_group_release(void *data)
{
	struct pkvm_tegra_group *group = data;

	mutex_lock(&group->smmu->group_lock);
	list_del(&group->list);
	mutex_unlock(&group->smmu->group_lock);
	kfree(group->sids);
	kfree(group);
}

static struct iommu_group *pkvm_tegra_device_group(struct device *dev)
{
	struct pkvm_tegra_master *master = dev_iommu_priv_get(dev);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pkvm_tegra_group *group, *match = NULL;
	u32 *sids;
	unsigned int i, num_sids;

	if (!master || !fwspec || !fwspec->num_ids)
		return ERR_PTR(-ENODEV);

	mutex_lock(&master->smmu->group_lock);
	list_for_each_entry(group, &master->smmu->groups, list) {
		for (i = 0; i < fwspec->num_ids; i++) {
			if (!pkvm_tegra_group_has_sid(group, fwspec->ids[i]))
				continue;
			if (match && match != group) {
				mutex_unlock(&master->smmu->group_lock);
				return ERR_PTR(-EINVAL);
			}
			match = group;
		}
	}

	if (match) {
		num_sids = match->num_sids;
		for (i = 0; i < fwspec->num_ids; i++)
			if (!pkvm_tegra_group_has_sid(match, fwspec->ids[i]))
				num_sids++;
		sids = krealloc_array(match->sids, num_sids, sizeof(*sids),
				      GFP_KERNEL);
		if (!sids) {
			mutex_unlock(&master->smmu->group_lock);
			return ERR_PTR(-ENOMEM);
		}
		match->sids = sids;
		for (i = 0; i < fwspec->num_ids; i++)
			if (!pkvm_tegra_group_has_sid(match, fwspec->ids[i]))
				match->sids[match->num_sids++] = fwspec->ids[i];
		group = match;
		mutex_unlock(&master->smmu->group_lock);
		return iommu_group_ref_get(group->group);
	}

	group = kzalloc_obj(*group, GFP_KERNEL);
	if (!group) {
		mutex_unlock(&master->smmu->group_lock);
		return ERR_PTR(-ENOMEM);
	}
	group->sids = kmemdup_array(fwspec->ids, fwspec->num_ids,
				     sizeof(*group->sids), GFP_KERNEL);
	if (!group->sids) {
		kfree(group);
		mutex_unlock(&master->smmu->group_lock);
		return ERR_PTR(-ENOMEM);
	}
	group->group = generic_device_group(dev);
	if (IS_ERR(group->group)) {
		struct iommu_group *ret = group->group;

		kfree(group->sids);
		kfree(group);
		mutex_unlock(&master->smmu->group_lock);
		return ret;
	}
	group->smmu = master->smmu;
	group->num_sids = fwspec->num_ids;
	iommu_group_set_iommudata(group->group, group,
				  pkvm_tegra_group_release);
	list_add_tail(&group->list, &master->smmu->groups);
	mutex_unlock(&master->smmu->group_lock);
	return group->group;
}

static int pkvm_tegra_default_domain(struct device *dev)
{
	return 0;
}

static const struct iommu_ops pkvm_tegra_iommu_ops = {
	.identity_domain = &pkvm_tegra_identity_domain,
	.blocked_domain = &pkvm_tegra_blocked_domain,
	.domain_alloc_paging = pkvm_tegra_domain_alloc_paging,
	.probe_device = pkvm_tegra_probe_device,
	.release_device = pkvm_tegra_release_device,
	.probe_finalize = pkvm_tegra_probe_finalize,
	.device_group = pkvm_tegra_device_group,
	.of_xlate = pkvm_tegra_of_xlate,
	.def_domain_type = pkvm_tegra_default_domain,
	.owner = THIS_MODULE,
};

static int pkvm_tegra_probe(struct platform_device *pdev)
{
	struct pkvm_tegra_smmu_device *hyp_smmu;
	struct pkvm_tegra_host_smmu *host_smmu;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	struct resource *res;
	u32 id0, id1, id2;
	unsigned int i;

	if (pkvm_tegra_smmu_current >= pkvm_tegra_smmu_count)
		return -ENOSPC;
	hyp_smmu = &pkvm_tegra_smmus[pkvm_tegra_smmu_current];
	host_smmu = devm_kzalloc(dev, sizeof(*host_smmu), GFP_KERNEL);
	if (!host_smmu)
		return -ENOMEM;

	for (i = 0; i < PKVM_TEGRA_SMMU_MAX_INSTANCES; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			break;
		hyp_smmu->mmio_addr[i] = res->start;
		if (!i)
			hyp_smmu->mmio_size = resource_size(res);
		else if (resource_size(res) != hyp_smmu->mmio_size)
			return dev_err_probe(dev, -EINVAL,
					     "SMMU instance sizes differ\n");
		hyp_smmu->num_instances++;
	}
	if (!hyp_smmu->num_instances)
		return -ENODEV;
	if (platform_get_resource(pdev, IORESOURCE_MEM,
				  PKVM_TEGRA_SMMU_MAX_INSTANCES))
		return dev_err_probe(dev, -E2BIG,
				     "too many mirrored SMMU instances\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);
	id0 = readl_relaxed(base + PKVM_SMMU_GR0_ID0);
	id1 = readl_relaxed(base + PKVM_SMMU_GR0_ID1);
	id2 = readl_relaxed(base + PKVM_SMMU_GR0_ID2);
	if (!(id0 & PKVM_SMMU_ID0_S2TS) ||
	    !(id0 & PKVM_SMMU_ID0_SMS) ||
	    (id0 & PKVM_SMMU_ID0_EXIDS) ||
	    !(id2 & PKVM_SMMU_ID2_PTFS_4K))
		return dev_err_probe(dev, -ENODEV,
				     "unsupported SMMU translation or stream format\n");

	hyp_smmu->pgshift = id1 & PKVM_SMMU_ID1_PAGESIZE ? 16 : 12;
	hyp_smmu->numpage = 1U <<
		(FIELD_GET(PKVM_SMMU_ID1_NUMPAGENDXB, id1) + 1);
	hyp_smmu->num_context_banks = FIELD_GET(PKVM_SMMU_ID1_NUMCB, id1);
	hyp_smmu->num_s2_context_banks =
		FIELD_GET(PKVM_SMMU_ID1_NUMS2CB, id1);
	hyp_smmu->num_mapping_groups = FIELD_GET(PKVM_SMMU_ID0_NUMSMRG, id0);
	hyp_smmu->streamid_mask =
		(1U << FIELD_GET(PKVM_SMMU_ID0_NUMSIDB, id0)) - 1;
	hyp_smmu->ias = pkvm_tegra_id_size(FIELD_GET(PKVM_SMMU_ID2_IAS, id2));
	hyp_smmu->oas = pkvm_tegra_id_size(FIELD_GET(PKVM_SMMU_ID2_OAS, id2));
	/*
	 * Match the Arm SMMU driver's coherency policy: firmware is the
	 * authority even when IDR0.CTTW advertises coherent table walks. This
	 * also covers implementations whose ID register is configured
	 * incorrectly. Tegra234 does not mark these SMMUs dma-coherent, so the
	 * hypervisor must publish page-table updates explicitly.
	 */
	hyp_smmu->coherent_walk = of_dma_is_coherent(dev->of_node);
	hyp_smmu->vmid16 = !!(id2 & PKVM_SMMU_ID2_VMID16);
	if ((hyp_smmu->pgshift != 12 && hyp_smmu->pgshift != 16) ||
	    hyp_smmu->mmio_size <
		((u64)2 * hyp_smmu->numpage << hyp_smmu->pgshift) ||
	    hyp_smmu->num_s2_context_banks > hyp_smmu->num_context_banks ||
	    hyp_smmu->num_context_banks > PKVM_TEGRA_SMMU_MAX_CONTEXT_BANKS ||
	    hyp_smmu->num_mapping_groups > PKVM_TEGRA_SMMU_MAX_SMRS ||
	    !hyp_smmu->num_mapping_groups || !hyp_smmu->ias || !hyp_smmu->oas)
		return dev_err_probe(dev, -ENODEV,
				     "unsupported Tegra SMMU configuration\n");

	host_smmu->dev = dev;
	host_smmu->id = pkvm_tegra_smmu_current;
	host_smmu->address_bits = min3(hyp_smmu->ias, hyp_smmu->oas,
				       get_kvm_ipa_limit());
	/*
	 * Keep the pKVM bring-up DMA aperture below 4 GiB.  Tegra234 clients
	 * using the top of their 39-bit DMA mask currently reach the memory
	 * controller as 0xffffffff00 without raising a context-bank fault,
	 * while clients allocated in the 32-bit aperture translate normally.
	 * Preserve the SMMU's full output address size so buffers may remain
	 * above 4 GiB; this limit applies only to device-visible IOVAs.
	 */
	host_smmu->address_bits = min(host_smmu->address_bits, 32U);
	host_smmu->mc = devm_tegra_memory_controller_get(dev);
	if (IS_ERR(host_smmu->mc))
		return PTR_ERR(host_smmu->mc);
	mutex_init(&host_smmu->group_lock);
	INIT_LIST_HEAD(&host_smmu->groups);
	platform_set_drvdata(pdev, host_smmu);
	pkvm_tegra_smmu_current++;
	return 0;
}

static const struct of_device_id pkvm_tegra_smmu_of_match[] = {
	{ .compatible = "nvidia,tegra234-smmu" },
	{ }
};

static struct platform_driver pkvm_tegra_smmu_driver = {
	.driver = {
		.name = "pkvm-tegra234-smmu",
		.of_match_table = pkvm_tegra_smmu_of_match,
	},
};

static int pkvm_tegra_register_iommu(struct device *dev, void *data)
{
	struct pkvm_tegra_host_smmu *smmu = dev_get_drvdata(dev);
	int ret;

	ret = iommu_device_sysfs_add(&smmu->iommu, dev, NULL,
				     "pkvm-tegra-smmu.%u", smmu->id);
	if (ret) {
		dev_err(dev, "failed to add pKVM IOMMU %llu to sysfs: %d\n",
			smmu->id, ret);
		return ret;
	}
	ret = iommu_device_register(&smmu->iommu, &pkvm_tegra_iommu_ops, dev);
	if (ret) {
		dev_err(dev, "failed to register pKVM IOMMU %llu: %d\n",
			smmu->id, ret);
		iommu_device_sysfs_remove(&smmu->iommu);
	}
	return ret;
}

static int pkvm_tegra_init_driver(void)
{
	struct kvm_iommu_ops *hyp_ops;
	struct pkvm_tegra_smmu_device **hyp_smmus;
	size_t *hyp_smmu_count;
	int ret;

	ret = platform_driver_probe(&pkvm_tegra_smmu_driver, pkvm_tegra_probe);
	if (ret)
		return ret;
	if (pkvm_tegra_smmu_current != pkvm_tegra_smmu_count)
		return -EUNATCH;

	hyp_smmus = (struct pkvm_tegra_smmu_device **)
		kvm_nvhe_sym(pkvm_tegra_smmu_devices);
	hyp_smmu_count = (size_t *)kvm_nvhe_sym(pkvm_tegra_smmu_count);
	*hyp_smmus = pkvm_tegra_smmus;
	*hyp_smmu_count = pkvm_tegra_smmu_count;
	hyp_ops = kern_hyp_va(lm_alias(kvm_nvhe_sym(pkvm_tegra_smmu_ops)));
	ret = kvm_iommu_register_hyp_ops(hyp_ops, &pkvm_tegra_hyp_driver);
	if (ret)
		return ret;
	ret = driver_for_each_device(&pkvm_tegra_smmu_driver.driver, NULL,
				     NULL, pkvm_tegra_register_iommu);
	if (ret)
		return ret;
	for (size_t iommu = 0; iommu < pkvm_tegra_smmu_count; iommu++) {
		for (u32 selector = 0; selector < 10; selector++) {
			u64 value0 = 0, value1 = 0;

			ret = kvm_iommu_debug_read(pkvm_tegra_hyp_driver, iommu,
						   selector, &value0, &value1);
			pr_info("tegra-pkvm-debug: iommu=%zu selector=%u ret=%d value0=%#llx value1=%#llx\n",
				iommu, selector, ret,
				(unsigned long long)value0,
				(unsigned long long)value1);
		}
	}
	return 0;
}

static int pkvm_tegra_get_iommu_id_by_of(struct device_node *np,
					 pkvm_handle_t *out_id)
{
	struct device *dev;
	struct pkvm_tegra_host_smmu *smmu;

	dev = driver_find_device_by_of_node(&pkvm_tegra_smmu_driver.driver, np);
	if (!dev)
		return -ENODEV;
	smmu = dev_get_drvdata(dev);
	*out_id = smmu->id;
	put_device(dev);
	return 0;
}

static int pkvm_tegra_device_num_ids(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	return fwspec ? fwspec->num_ids : 0;
}

static int pkvm_tegra_device_id(struct device *dev, u32 idx,
				pkvm_handle_t *out_iommu, u32 *out_sid)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pkvm_tegra_master *master = dev_iommu_priv_get(dev);

	if (!fwspec || !master)
		return -ENODEV;
	if (idx >= fwspec->num_ids)
		return -ENOENT;
	*out_iommu = master->smmu->id;
	*out_sid = fwspec->ids[idx];
	return 0;
}

static int pkvm_tegra_prepare_protected_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct pkvm_tegra_host_smmu *smmu;

	if (!fwspec)
		return 0;
	smmu = pkvm_tegra_smmu_from_fwnode(fwspec->iommu_fwnode);
	if (!smmu)
		return 0;

	return tegra_mc_lock_device_stream_id(smmu->mc, dev);
}

static struct kvm_iommu_driver pkvm_tegra_driver = {
	.init_driver = pkvm_tegra_init_driver,
	.get_iommu_id_by_of = pkvm_tegra_get_iommu_id_by_of,
	.get_device_iommu_num_ids = pkvm_tegra_device_num_ids,
	.get_device_iommu_id = pkvm_tegra_device_id,
	.prepare_protected_device = pkvm_tegra_prepare_protected_device,
};

static int __init pkvm_tegra_register(void)
{
	struct device_node *np;
	size_t bytes;
	int ret;

	if (!is_protected_kvm_enabled())
		return 0;
	for_each_compatible_node(np, NULL, "nvidia,tegra234-smmu")
		if (of_device_is_available(np))
			pkvm_tegra_smmu_count++;
	if (!pkvm_tegra_smmu_count)
		return 0;
	if (pkvm_tegra_smmu_count > PKVM_TEGRA_SMMU_MAX_DEVICES)
		return -E2BIG;

	bytes = PAGE_ALIGN(array_size(pkvm_tegra_smmu_count,
				      sizeof(*pkvm_tegra_smmus)));
	pkvm_tegra_smmus = alloc_pages_exact(bytes, GFP_KERNEL | __GFP_ZERO);
	if (!pkvm_tegra_smmus)
		return -ENOMEM;

	ret = kvm_iommu_register_driver(&pkvm_tegra_driver,
					host_s2_pgtable_pages() + 256);
	if (ret)
		free_pages_exact(pkvm_tegra_smmus, bytes);
	return ret;
}
core_initcall(pkvm_tegra_register);
