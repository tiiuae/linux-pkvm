// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tegra234 SMMUv2 backend for protected KVM.
 *
 * Copyright (C) 2026 TII (SSRC) and the Ghaf contributors
 */

#include <asm/kvm_hyp.h>
#include <asm/kvm_pgtable.h>
#include <asm/kvm_pkvm.h>
#include <asm/cacheflush.h>

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/iommu.h>

#include <kvm/device.h>
#include <kvm/tegra-smmu-pkvm.h>

#include <nvhe/alloc.h>
#include <nvhe/gfp.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/spinlock.h>

#define PKVM_TEGRA_TLB_SPINS		1000000
#define PKVM_TEGRA_MGBE_RESET_SPINS	2000000

/* Temporary boot diagnostics for the nvidia-jetson-orin-agx-pkvm-debug target. */
#define PKVM_TEGRA_DIAG_INIT_PARAMS_DONATE	(-1001)
#define PKVM_TEGRA_DIAG_INIT_PARAMS		(-1002)
#define PKVM_TEGRA_DIAG_INIT_MMIO_ALIGN		(-1100)
#define PKVM_TEGRA_DIAG_INIT_MMIO_DONATE	(-1200)
#define PKVM_TEGRA_DIAG_INIT_RESET		(-1300)
#define PKVM_TEGRA_DIAG_INIT_PGTABLE		(-1400)
#define PKVM_TEGRA_DIAG_INIT_PVIOMMU		(-1500)
#define PKVM_TEGRA_DIAG_SNAPSHOT_MAP		(-200000000)
#define PKVM_TEGRA_DIAG_SNAPSHOT_TLB		(-400000000)

#define PKVM_TEGRA_MGBE0_HV_BASE	0x06800000
#define PKVM_TEGRA_MGBE0_MAC_BASE	0x06810000

#define PKVM_TEGRA_MGBE_WRAP_INTR_ENABLE	0x8704
#define PKVM_TEGRA_MGBE_DMA_MODE		0x3000
#define PKVM_TEGRA_MGBE_DMA_MODE_SWR	BIT(0)

struct pkvm_tegra_hyp_smmu {
	struct pkvm_tegra_smmu_device *params;
	void __iomem *base[PKVM_TEGRA_SMMU_MAX_INSTANCES];
	hyp_spinlock_t lock;
	DECLARE_BITMAP(context_map, PKVM_TEGRA_SMMU_MAX_CONTEXT_BANKS);
	u16 smr_sid[PKVM_TEGRA_SMMU_MAX_SMRS];
	u8 smr_cb[PKVM_TEGRA_SMMU_MAX_SMRS];
	bool smr_valid[PKVM_TEGRA_SMMU_MAX_SMRS];
};

struct pkvm_tegra_hyp_domain {
	struct kvm_pgtable pgt;
	struct kvm_s2_mmu mmu;
	struct pkvm_tegra_hyp_smmu *smmu;
	hyp_spinlock_t lock;
	u16 vmid;
	u8 cb;
};

size_t __ro_after_init pkvm_tegra_smmu_count;
struct pkvm_tegra_smmu_device *pkvm_tegra_smmu_devices;

static struct pkvm_tegra_hyp_smmu
	tegra_smmus[PKVM_TEGRA_SMMU_MAX_DEVICES];
static struct pkvm_tegra_hyp_domain tegra_identity_domain;
static struct kvm_pgtable_mm_ops tegra_identity_mm_ops;
static struct kvm_pgtable_mm_ops tegra_domain_mm_ops;
static bool tegra_noncoherent_walk;
static bool tegra_snapshotting;

struct pkvm_tegra_mgbe {
	phys_addr_t hv_base;
	phys_addr_t mac_base;
};

static struct pkvm_tegra_mgbe tegra_mgbe0 = {
	.hv_base = PKVM_TEGRA_MGBE0_HV_BASE,
	.mac_base = PKVM_TEGRA_MGBE0_MAC_BASE,
};

static int tegra_mgbe_reset(void *cookie, bool host_to_guest)
{
	struct pkvm_tegra_mgbe *mgbe = cookie;
	void __iomem *mac = hyp_phys_to_virt(mgbe->mac_base);
	u32 value;
	unsigned int spin;

	(void)host_to_guest;

	/* The guest driver restores this wrapper interrupt gate. */
	writel_relaxed(0, mac + PKVM_TEGRA_MGBE_WRAP_INTR_ENABLE);
	value = readl_relaxed(mac + PKVM_TEGRA_MGBE_DMA_MODE);
	writel_relaxed(value | PKVM_TEGRA_MGBE_DMA_MODE_SWR,
		       mac + PKVM_TEGRA_MGBE_DMA_MODE);

	for (spin = 0; spin < PKVM_TEGRA_MGBE_RESET_SPINS; spin++) {
		value = readl_relaxed(mac + PKVM_TEGRA_MGBE_DMA_MODE);
		if (!(value & PKVM_TEGRA_MGBE_DMA_MODE_SWR))
			return 0;
		cpu_relax();
	}

	return -ETIMEDOUT;
}

static struct pkvm_device_ops tegra_mgbe_ops = {
	.reset = tegra_mgbe_reset,
};

static void tegra_init_devices(void)
{
	int ret;

	ret = pkvm_device_register_ops(tegra_mgbe0.hv_base, &tegra_mgbe_ops,
				       &tegra_mgbe0);
	WARN_ON(ret && ret != -ENODEV);
}

static void __iomem *tegra_smmu_page(struct pkvm_tegra_hyp_smmu *smmu,
				     unsigned int instance,
				     unsigned int page)
{
	return smmu->base[instance] + (page << smmu->params->pgshift);
}

static void __iomem *tegra_smmu_cb(struct pkvm_tegra_hyp_smmu *smmu,
				   unsigned int instance, unsigned int cb)
{
	return tegra_smmu_page(smmu, instance, smmu->params->numpage + cb);
}

static void tegra_smmu_write(struct pkvm_tegra_hyp_smmu *smmu,
			     unsigned int page, u32 offset, u32 value)
{
	unsigned int i;

	for (i = 0; i < smmu->params->num_instances; i++)
		writel_relaxed(value, tegra_smmu_page(smmu, i, page) + offset);
}

static void tegra_smmu_cb_write(struct pkvm_tegra_hyp_smmu *smmu,
				unsigned int cb, u32 offset, u32 value)
{
	unsigned int i;

	for (i = 0; i < smmu->params->num_instances; i++)
		writel_relaxed(value, tegra_smmu_cb(smmu, i, cb) + offset);
}

static void tegra_smmu_cb_writeq(struct pkvm_tegra_hyp_smmu *smmu,
				 unsigned int cb, u32 offset, u64 value)
{
	unsigned int i;

	for (i = 0; i < smmu->params->num_instances; i++)
		writeq_relaxed(value, tegra_smmu_cb(smmu, i, cb) + offset);
}

static int tegra_smmu_tlb_sync(struct pkvm_tegra_hyp_smmu *smmu)
{
	unsigned int spin, instance;
	u32 active;

	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_TLBGSYNC, 0);
	for (spin = 0; spin < PKVM_TEGRA_TLB_SPINS; spin++) {
		active = 0;
		for (instance = 0; instance < smmu->params->num_instances;
		     instance++)
			active |= readl_relaxed(tegra_smmu_page(smmu, instance, 0) +
						PKVM_SMMU_GR0_TLBGSTATUS);
		if (!(active & PKVM_SMMU_TLBGSTATUS_ACTIVE))
			return 0;
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static int tegra_smmu_flush_vmid(struct pkvm_tegra_hyp_smmu *smmu, u16 vmid)
{
	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_TLBIVMID, vmid);
	return tegra_smmu_tlb_sync(smmu);
}

static void *tegra_atomic_zalloc_page(void *arg)
{
	return kvm_iommu_donate_pages_atomic(0);
}

static void *tegra_atomic_zalloc_pages_exact(size_t size)
{
	if (size != PAGE_SIZE)
		return NULL;
	return kvm_iommu_donate_pages_atomic(0);
}

static void tegra_atomic_free_pages_exact(void *addr, size_t size)
{
	WARN_ON(size != PAGE_SIZE);
	kvm_iommu_reclaim_pages_atomic(addr);
}

static int tegra_page_count(void *addr)
{
	/* Never reclaim an empty lower table: Tegra234 walk caches are stale. */
	return max(2, hyp_page_count(addr));
}

static void tegra_atomic_free_unlinked(void *addr, s8 level)
{
	kvm_pgtable_stage2_free_unlinked(&tegra_identity_mm_ops, addr, level);
}

static void *tegra_domain_zalloc_page(void *arg)
{
	return kvm_iommu_donate_page();
}

static void *tegra_domain_zalloc_pages_exact(size_t size)
{
	if (size != PAGE_SIZE)
		return NULL;
	return kvm_iommu_donate_page();
}

static void tegra_domain_free_pages_exact(void *addr, size_t size)
{
	WARN_ON(size != PAGE_SIZE);
	kvm_iommu_reclaim_page(addr);
}

static void tegra_domain_free_unlinked(void *addr, s8 level)
{
	kvm_pgtable_stage2_free_unlinked(&tegra_domain_mm_ops, addr, level);
}

static bool tegra_force_pte(u64 addr, u64 end, enum kvm_pgtable_prot prot)
{
	/*
	 * Build the initial RAM identity map with blocks, as the Android pKVM
	 * IOMMU backends do. Subsequent host stage-2 permission changes remain
	 * page-granular so that replacing a block cannot expose neighbouring
	 * pages and so the Tegra walk-cache workaround never frees a split table.
	 */
	return !tegra_snapshotting;
}

static int tegra_ps(unsigned int address_bits)
{
	switch (address_bits) {
	case 32:
		return 0;
	case 36:
		return 1;
	case 40:
		return 2;
	case 42:
		return 3;
	case 44:
		return 4;
	case 48:
		return 5;
	default:
		return -EINVAL;
	}
}

static unsigned int tegra_cpu_pa_bits(void)
{
	u64 parange = kvm_get_parange(id_aa64mmfr0_el1_sys_val);

	return id_aa64mmfr0_parange_to_phys_shift(parange);
}

static unsigned int
tegra_smmu_address_bits(struct pkvm_tegra_hyp_smmu *smmu)
{
	return min(tegra_cpu_pa_bits(),
		   min(smmu->params->ias, smmu->params->oas));
}

static u64 tegra_vtcr(unsigned int address_bits)
{
	u64 vtcr = kvm_get_vtcr(id_aa64mmfr0_el1_sys_val,
				id_aa64mmfr1_el1_sys_val, address_bits);

	vtcr &= ~PKVM_SMMU_VTCR_PS;
	vtcr |= FIELD_PREP(PKVM_SMMU_VTCR_PS, tegra_ps(address_bits));
	return vtcr;
}

static int tegra_init_pgtable(struct pkvm_tegra_hyp_domain *domain,
			      struct kvm_pgtable_mm_ops *mm_ops,
			      unsigned int address_bits, bool identity)
{
	int ret;

	memset(&domain->mmu, 0, sizeof(domain->mmu));
	domain->mmu.vtcr = tegra_vtcr(address_bits);
	domain->mmu.pgt = &domain->pgt;
	atomic64_set(&domain->mmu.vmid.id, 0);
	ret = __kvm_pgtable_stage2_init(&domain->pgt, &domain->mmu, mm_ops,
					identity ? KVM_PGTABLE_S2_IDMAP : 0,
					tegra_force_pte);
	if (ret)
		return ret;
	domain->mmu.pgd_phys = hyp_virt_to_phys(domain->pgt.pgd);
	return 0;
}

static u32 tegra_domain_vtcr(struct pkvm_tegra_hyp_domain *domain)
{
	return PKVM_SMMU_VTCR_RES1 | (domain->mmu.vtcr & GENMASK(18, 0));
}

static int tegra_sync_pte(const struct kvm_pgtable_visit_ctx *ctx,
			  enum kvm_pgtable_walk_flags visit)
{
	void *child;

	if (visit != KVM_PGTABLE_WALK_TABLE_POST)
		return 0;

	child = ctx->mm_ops->phys_to_virt(kvm_pte_to_phys(ctx->old));
	dcache_clean_inval_poc((unsigned long)child,
			       (unsigned long)child + PAGE_SIZE);
	return 0;
}

static void tegra_sync_pgtable(struct pkvm_tegra_hyp_domain *domain,
			       u64 start, u64 size)
{
	struct kvm_pgtable_walker walker = {
		.cb = tegra_sync_pte,
		/* Publish child entries before their parent table entries. */
		.flags = KVM_PGTABLE_WALK_LEAF | KVM_PGTABLE_WALK_TABLE_POST,
	};

	if (!tegra_noncoherent_walk)
		return;
	WARN_ON(kvm_pgtable_walk(&domain->pgt, start, size, &walker));
	/* The root table has no parent entry for the post-order walker. */
	dcache_clean_inval_poc((unsigned long)domain->pgt.pgd,
			       (unsigned long)domain->pgt.pgd + PAGE_SIZE);
	dsb(sy);
}

static void tegra_program_context(struct pkvm_tegra_hyp_smmu *smmu,
				  struct pkvm_tegra_hyp_domain *domain)
{
	u32 cba2r = PKVM_SMMU_CBA2R_VA64;
	u32 cbar = FIELD_PREP(PKVM_SMMU_CBAR_TYPE,
			      PKVM_SMMU_CBAR_TYPE_S2);

	tegra_sync_pgtable(domain, 0, BIT(domain->pgt.ia_bits));
	tegra_smmu_cb_write(smmu, domain->cb, PKVM_SMMU_CB_SCTLR, 0);
	if (smmu->params->vmid16)
		cba2r |= FIELD_PREP(PKVM_SMMU_CBA2R_VMID16, domain->vmid);
	else
		cbar |= FIELD_PREP(PKVM_SMMU_CBAR_VMID, domain->vmid);
	tegra_smmu_write(smmu, 1, PKVM_SMMU_GR1_CBA2R(domain->cb), cba2r);
	tegra_smmu_write(smmu, 1, PKVM_SMMU_GR1_CBAR(domain->cb), cbar);
	tegra_smmu_cb_write(smmu, domain->cb, PKVM_SMMU_CB_TCR,
			    tegra_domain_vtcr(domain));
	tegra_smmu_cb_writeq(smmu, domain->cb, PKVM_SMMU_CB_TTBR0,
			     domain->mmu.pgd_phys);
	tegra_smmu_cb_write(smmu, domain->cb, PKVM_SMMU_CB_FSR, ~0U);
	dsb(ishst);
	tegra_smmu_cb_write(smmu, domain->cb, PKVM_SMMU_CB_SCTLR,
			    PKVM_SMMU_SCTLR_AFE | PKVM_SMMU_SCTLR_TRE |
			    PKVM_SMMU_SCTLR_M);
}

static struct pkvm_tegra_hyp_smmu *tegra_smmu_from_id(pkvm_handle_t id)
{
	if (id >= pkvm_tegra_smmu_count)
		return NULL;
	return &tegra_smmus[array_index_nospec(id, pkvm_tegra_smmu_count)];
}

static int tegra_find_smr(struct pkvm_tegra_hyp_smmu *smmu, u32 sid)
{
	int free = -ENOSPC;
	unsigned int i;

	for (i = 0; i < smmu->params->num_mapping_groups; i++) {
		if (smmu->smr_valid[i] && smmu->smr_sid[i] == sid)
			return i;
		if (!smmu->smr_valid[i] && free < 0)
			free = i;
	}
	return free;
}

static void tegra_block_smr(struct pkvm_tegra_hyp_smmu *smmu,
			    unsigned int smr)
{
	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_SMR(smr), 0);
	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_S2CR(smr),
			 FIELD_PREP(PKVM_SMMU_S2CR_TYPE,
				    PKVM_SMMU_S2CR_TYPE_FAULT));
	smmu->smr_valid[smr] = false;
}

static int tegra_route_sid(struct pkvm_tegra_hyp_smmu *smmu, u32 sid, u8 cb)
{
	int smr;

	if (sid & ~smmu->params->streamid_mask)
		return -ERANGE;
	smr = tegra_find_smr(smmu, sid);
	if (smr < 0)
		return smr;
	if (smmu->smr_valid[smr])
		return smmu->smr_cb[smr] == cb ? 0 : -EBUSY;

	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_S2CR(smr),
			 FIELD_PREP(PKVM_SMMU_S2CR_TYPE,
				    PKVM_SMMU_S2CR_TYPE_TRANS) |
			 FIELD_PREP(PKVM_SMMU_S2CR_CBNDX, cb));
	dsb(ishst);
	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_SMR(smr),
			 PKVM_SMMU_SMR_VALID |
			 FIELD_PREP(PKVM_SMMU_SMR_ID, sid));
	smmu->smr_sid[smr] = sid;
	smmu->smr_cb[smr] = cb;
	smmu->smr_valid[smr] = true;
	return 0;
}

static int tegra_unroute_sid(struct pkvm_tegra_hyp_smmu *smmu, u32 sid,
			     int expected_cb)
{
	int smr = tegra_find_smr(smmu, sid);

	if (smr < 0 || !smmu->smr_valid[smr])
		return -ENOENT;
	if (expected_cb >= 0 && smmu->smr_cb[smr] != expected_cb)
		return -EPERM;
	tegra_block_smr(smmu, smr);
	return 0;
}

static int tegra_reset_smmu(struct pkvm_tegra_hyp_smmu *smmu)
{
	unsigned int i;
	u32 scr0;

	scr0 = readl_relaxed(smmu->base[0] + PKVM_SMMU_GR0_SCR0);
	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_SCR0,
			 scr0 | PKVM_SMMU_SCR0_CLIENTPD);

	for (i = 0; i < smmu->params->num_context_banks; i++)
		tegra_smmu_cb_write(smmu, i, PKVM_SMMU_CB_SCTLR, 0);
	for (i = 0; i < smmu->params->num_mapping_groups; i++)
		tegra_block_smr(smmu, i);

	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_TLBIALLH, 0);
	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_TLBIALLNSNH, 0);
	if (tegra_smmu_tlb_sync(smmu))
		return -ETIMEDOUT;

	for (i = 0; i < smmu->params->num_instances; i++) {
		u32 gfsr = readl_relaxed(tegra_smmu_page(smmu, i, 0) +
					 PKVM_SMMU_GR0_GFSR);
		writel_relaxed(gfsr, tegra_smmu_page(smmu, i, 0) +
				     PKVM_SMMU_GR0_GFSR);
	}

	scr0 &= ~(PKVM_SMMU_SCR0_CLIENTPD);
	scr0 &= ~(PKVM_SMMU_SCR0_BSU | PKVM_SMMU_SCR0_FB |
		   PKVM_SMMU_SCR0_GCFGFIE | PKVM_SMMU_SCR0_GFIE |
		   PKVM_SMMU_SCR0_VMID16EN);
	scr0 |= PKVM_SMMU_SCR0_PTM | PKVM_SMMU_SCR0_VMIDPNE |
		 PKVM_SMMU_SCR0_USFCFG | PKVM_SMMU_SCR0_GCFGFRE |
		 PKVM_SMMU_SCR0_GFRE;
	if (smmu->params->vmid16)
		scr0 |= PKVM_SMMU_SCR0_VMID16EN;
	tegra_smmu_write(smmu, 0, PKVM_SMMU_GR0_SCR0, scr0);
	return 0;
}

static int tegra_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
	struct pkvm_tegra_hyp_domain *domain = &tegra_identity_domain;
	enum kvm_pgtable_prot pgt_prot = 0;
	unsigned int i;
	int ret = 0;

	end = min_t(phys_addr_t, end, BIT(domain->pgt.ia_bits));
	if (start >= end)
		return 0;
	/*
	 * Empty non-memory leaves cover most of the host IPA space during the
	 * initial stage-2 snapshot. They are not DMA targets and mirroring them
	 * either consumes the atomic page-table pool or requires block mappings
	 * that would later require unsafe splitting. Mirror RAM with blocks during
	 * the snapshot, then keep runtime permission updates page-granular.
	 */
	if (tegra_snapshotting && (prot & IOMMU_MMIO))
		return 0;
	if (prot & IOMMU_READ)
		pgt_prot |= KVM_PGTABLE_PROT_R;
	if (prot & IOMMU_WRITE)
		pgt_prot |= KVM_PGTABLE_PROT_W;
	if (prot & IOMMU_MMIO)
		pgt_prot |= KVM_PGTABLE_PROT_DEVICE;

	hyp_spin_lock(&domain->lock);
	if (pgt_prot)
		ret = kvm_pgtable_stage2_map(&domain->pgt, start, end - start,
					     start, pgt_prot, (void *)1, 0);
	else
		ret = kvm_pgtable_stage2_unmap(&domain->pgt, start, end - start);
	if (ret && tegra_snapshotting)
		ret = PKVM_TEGRA_DIAG_SNAPSHOT_MAP - (int)(start >> 21);
	if (!ret) {
		tegra_sync_pgtable(domain, start, end - start);
		for (i = 0; i < pkvm_tegra_smmu_count; i++) {
			ret = tegra_smmu_flush_vmid(&tegra_smmus[i], 0);
			if (ret && tegra_snapshotting) {
				ret = PKVM_TEGRA_DIAG_SNAPSHOT_TLB - (int)i;
				break;
			}
		}
	}
	hyp_spin_unlock(&domain->lock);
	return ret;
}

static int tegra_alloc_domain(pkvm_handle_t iommu_id,
			      struct kvm_hyp_iommu_domain *core_domain,
			      int type)
{
	struct pkvm_tegra_hyp_smmu *smmu = tegra_smmu_from_id(iommu_id);
	struct pkvm_tegra_hyp_domain *domain;
	unsigned int cb;
	int ret;

	if (!smmu || type != 1)
		return -EINVAL;
	domain = hyp_alloc(sizeof(*domain));
	if (!domain) {
		kvm_iommu_request_hyp_alloc();
		return -ENOMEM;
	}
	memset(domain, 0, sizeof(*domain));
	domain->smmu = smmu;
	hyp_spin_lock_init(&domain->lock);

	hyp_spin_lock(&smmu->lock);
	for (cb = 1; cb < smmu->params->num_context_banks; cb++)
		if (!test_bit(cb, smmu->context_map))
			break;
	if (cb >= smmu->params->num_context_banks) {
		ret = -ENOSPC;
		goto err_unlock;
	}
	set_bit(cb, smmu->context_map);
	domain->cb = cb;
	domain->vmid = cb;
	hyp_spin_unlock(&smmu->lock);

	ret = tegra_init_pgtable(domain, &tegra_domain_mm_ops,
				 tegra_smmu_address_bits(smmu), false);
	if (ret)
		goto err_cb;
	tegra_program_context(smmu, domain);
	core_domain->priv = domain;
	return 0;

err_cb:
	hyp_spin_lock(&smmu->lock);
	clear_bit(cb, smmu->context_map);
err_unlock:
	hyp_spin_unlock(&smmu->lock);
	hyp_free(domain);
	return ret;
}

static int tegra_unpin_walker(const struct kvm_pgtable_visit_ctx *ctx,
			      enum kvm_pgtable_walk_flags visit)
{
	phys_addr_t phys = kvm_pte_to_phys(ctx->old);
	size_t size = kvm_granule_size(ctx->level);

	if (kvm_pte_valid(ctx->old) &&
	    !(ctx->level < KVM_PGTABLE_LAST_LEVEL &&
	      (ctx->old & KVM_PTE_TYPE)))
		WARN_ON(iommu_pkvm_unuse_dma(phys, size));
	return 0;
}

static void tegra_free_domain(struct kvm_hyp_iommu_domain *core_domain)
{
	struct pkvm_tegra_hyp_domain *domain = core_domain->priv;
	struct kvm_pgtable_walker walker = {
		.cb = tegra_unpin_walker,
		.flags = KVM_PGTABLE_WALK_LEAF,
	};

	if (!domain)
		return;
	tegra_smmu_cb_write(domain->smmu, domain->cb,
			    PKVM_SMMU_CB_SCTLR, 0);
	WARN_ON(tegra_smmu_flush_vmid(domain->smmu, domain->vmid));
	WARN_ON(kvm_pgtable_walk(&domain->pgt, 0, BIT(domain->pgt.ia_bits),
				 &walker));
	kvm_pgtable_stage2_destroy(&domain->pgt);
	hyp_spin_lock(&domain->smmu->lock);
	clear_bit(domain->cb, domain->smmu->context_map);
	hyp_spin_unlock(&domain->smmu->lock);
	hyp_free(domain);
	core_domain->priv = NULL;
}

static int tegra_attach_dev(pkvm_handle_t iommu_id,
			    struct kvm_hyp_iommu_domain *core_domain,
			    pkvm_handle_t sid, u32 pasid, u32 pasid_bits,
			    unsigned long flags)
{
	struct pkvm_tegra_hyp_smmu *smmu = tegra_smmu_from_id(iommu_id);
	struct pkvm_tegra_hyp_domain *domain = core_domain->priv;
	int ret;

	if (!smmu || !domain || domain->smmu != smmu || pasid)
		return -EINVAL;
	hyp_spin_lock(&smmu->lock);
	ret = tegra_route_sid(smmu, sid, domain->cb);
	hyp_spin_unlock(&smmu->lock);
	return ret;
}

static int tegra_detach_dev(pkvm_handle_t iommu_id,
			    struct kvm_hyp_iommu_domain *core_domain,
			    pkvm_handle_t sid, u32 pasid)
{
	struct pkvm_tegra_hyp_smmu *smmu = tegra_smmu_from_id(iommu_id);
	struct pkvm_tegra_hyp_domain *domain = core_domain->priv;
	int ret;

	if (!smmu || !domain || pasid)
		return -EINVAL;
	hyp_spin_lock(&smmu->lock);
	ret = tegra_unroute_sid(smmu, sid, domain->cb);
	hyp_spin_unlock(&smmu->lock);
	return ret;
}

static int tegra_map_pages(struct kvm_hyp_iommu_domain *core_domain,
			   unsigned long iova, phys_addr_t paddr,
			   size_t pgsize, size_t pgcount, int prot,
			   size_t *total_mapped)
{
	struct pkvm_tegra_hyp_domain *domain = core_domain->priv;
	enum kvm_pgtable_prot pgt_prot = 0;
	size_t size = pgsize * pgcount;
	size_t mapped = 0;
	bool stale = false;
	int ret;

	*total_mapped = 0;
	if (!domain || pgsize != PAGE_SIZE || !size)
		return -EINVAL;
	if (prot & IOMMU_READ)
		pgt_prot |= KVM_PGTABLE_PROT_R;
	if (prot & IOMMU_WRITE)
		pgt_prot |= KVM_PGTABLE_PROT_W;
	if (prot & IOMMU_MMIO)
		pgt_prot |= KVM_PGTABLE_PROT_DEVICE;
	if (!pgt_prot)
		return -EINVAL;

	ret = iommu_pkvm_use_dma(paddr, size);
	if (ret)
		return ret;
	hyp_spin_lock(&domain->lock);
	while (mapped < size) {
		ret = kvm_pgtable_stage2_map(&domain->pgt, iova + mapped,
					     PAGE_SIZE, paddr + mapped,
					     pgt_prot, (void *)1, 0);
		if (ret)
			break;
		mapped += PAGE_SIZE;
	}
	if (mapped) {
		/* Publish successful leaves, including a partial -ENOMEM map. */
		tegra_sync_pgtable(domain, iova, mapped);
		if (tegra_smmu_flush_vmid(domain->smmu, domain->vmid)) {
			/* Keep the DMA pins if stale translations cannot be excluded. */
			if (!kvm_pgtable_stage2_unmap(&domain->pgt, iova, mapped)) {
				tegra_sync_pgtable(domain, iova, mapped);
				if (!tegra_smmu_flush_vmid(domain->smmu,
							    domain->vmid))
					mapped = 0;
				else
					stale = true;
			} else {
				stale = true;
			}
			ret = -EIO;
		}
	}
	hyp_spin_unlock(&domain->lock);
	if (mapped < size)
		WARN_ON(iommu_pkvm_unuse_dma(paddr + mapped, size - mapped));
	if (stale) {
		*total_mapped = 0;
		return ret;
	}
	*total_mapped = mapped;
	return ret;
}

static size_t tegra_unmap_pages(struct kvm_hyp_iommu_domain *core_domain,
				unsigned long iova, size_t pgsize,
				size_t pgcount,
				struct iommu_iotlb_gather *gather)
{
	struct pkvm_tegra_hyp_domain *domain = core_domain->priv;
	size_t unmapped = 0;

	if (!domain || pgsize != PAGE_SIZE)
		return 0;
	hyp_spin_lock(&domain->lock);
	while (pgcount--) {
		kvm_pte_t pte;
		s8 level;
		phys_addr_t phys;

		if (kvm_pgtable_get_leaf(&domain->pgt, iova, &pte, &level) ||
		    !kvm_pte_valid(pte) || level != KVM_PGTABLE_LAST_LEVEL)
			break;
		phys = kvm_pte_to_phys(pte);
		if (kvm_pgtable_stage2_unmap(&domain->pgt, iova, PAGE_SIZE))
			break;
		tegra_sync_pgtable(domain, iova, PAGE_SIZE);
		if (tegra_smmu_flush_vmid(domain->smmu, domain->vmid))
			break;
		WARN_ON(iommu_pkvm_unuse_dma(phys, PAGE_SIZE));
		iova += PAGE_SIZE;
		unmapped += PAGE_SIZE;
	}
	hyp_spin_unlock(&domain->lock);
	return unmapped;
}

static phys_addr_t tegra_iova_to_phys(struct kvm_hyp_iommu_domain *core_domain,
				      unsigned long iova)
{
	struct pkvm_tegra_hyp_domain *domain = core_domain->priv;
	kvm_pte_t pte;
	s8 level;

	if (!domain || kvm_pgtable_get_leaf(&domain->pgt, iova, &pte, &level) ||
	    !kvm_pte_valid(pte))
		return 0;
	return kvm_pte_to_phys(pte) + (iova & (kvm_granule_size(level) - 1));
}

static int tegra_set_identity(pkvm_handle_t iommu_id, pkvm_handle_t sid,
			      bool state, unsigned long flags)
{
	struct pkvm_tegra_hyp_smmu *smmu = tegra_smmu_from_id(iommu_id);
	int ret;

	if (!smmu)
		return -ENODEV;
	hyp_spin_lock(&smmu->lock);
	if (state)
		ret = tegra_route_sid(smmu, sid, tegra_identity_domain.cb);
	else
		ret = tegra_unroute_sid(smmu, sid, tegra_identity_domain.cb);
	hyp_spin_unlock(&smmu->lock);
	return ret;
}

static int tegra_dev_block_dma(pkvm_handle_t iommu_id, u32 sid,
			       bool host_to_guest)
{
	struct pkvm_tegra_hyp_smmu *smmu = tegra_smmu_from_id(iommu_id);
	int smr, ret = 0;

	if (!smmu)
		return -ENODEV;
	hyp_spin_lock(&smmu->lock);
	smr = tegra_find_smr(smmu, sid);
	if (host_to_guest) {
		if (smr >= 0 && smmu->smr_valid[smr])
			ret = -EBUSY;
	} else if (smr >= 0 && smmu->smr_valid[smr]) {
		tegra_block_smr(smmu, smr);
	}
	hyp_spin_unlock(&smmu->lock);
	return ret;
}

static int tegra_iommu_token(pkvm_handle_t iommu_id, u64 *out_token)
{
	struct pkvm_tegra_hyp_smmu *smmu = tegra_smmu_from_id(iommu_id);

	if (!smmu)
		return -ENODEV;
	*out_token = smmu->params->mmio_addr[0];
	return 0;
}

static void tegra_snapshot_start(void)
{
	tegra_snapshotting = true;
}

static void tegra_snapshot_end(void)
{
	tegra_snapshotting = false;
}

static int tegra_init(pkvm_handle_t driver_id)
{
	struct pkvm_tegra_smmu_device *params;
	unsigned int address_bits = tegra_cpu_pa_bits();
	u64 params_pfn;
	size_t params_size;
	unsigned int i, instance, word;
	int ret;

	if (!pkvm_tegra_smmu_count ||
	    pkvm_tegra_smmu_count > PKVM_TEGRA_SMMU_MAX_DEVICES)
		return -ENODEV;
	pkvm_tegra_smmu_devices = kern_hyp_va(pkvm_tegra_smmu_devices);
	params_size = PAGE_ALIGN(pkvm_tegra_smmu_count *
				 sizeof(*pkvm_tegra_smmu_devices));
	params_pfn = hyp_virt_to_phys(pkvm_tegra_smmu_devices) >> PAGE_SHIFT;
	ret = __pkvm_host_donate_hyp(params_pfn, params_size >> PAGE_SHIFT);
	if (ret)
		return PKVM_TEGRA_DIAG_INIT_PARAMS_DONATE;

	for (i = 0; i < pkvm_tegra_smmu_count; i++) {
		struct pkvm_tegra_hyp_smmu *smmu = &tegra_smmus[i];

		params = &pkvm_tegra_smmu_devices[i];
		if ((params->pgshift != 12 && params->pgshift != 16) ||
		    !params->num_instances ||
		    params->num_instances > PKVM_TEGRA_SMMU_MAX_INSTANCES ||
		    params->num_context_banks >
			PKVM_TEGRA_SMMU_MAX_CONTEXT_BANKS ||
		    params->num_s2_context_banks > params->num_context_banks ||
		    params->num_mapping_groups > PKVM_TEGRA_SMMU_MAX_SMRS ||
		    tegra_ps(min(params->ias, params->oas)) < 0)
			return PKVM_TEGRA_DIAG_INIT_PARAMS - (int)i;
		smmu->params = params;
		tegra_noncoherent_walk |= !params->coherent_walk;
		hyp_spin_lock_init(&smmu->lock);
		for (word = 0; word < ARRAY_SIZE(smmu->context_map); word++)
			smmu->context_map[word] = 0;
		set_bit(0, smmu->context_map);
		address_bits = min(address_bits, min(params->ias, params->oas));

		for (instance = 0; instance < params->num_instances; instance++) {
			u64 pfn = params->mmio_addr[instance] >> PAGE_SHIFT;
			u64 pages = params->mmio_size >> PAGE_SHIFT;

			if (!PAGE_ALIGNED(params->mmio_addr[instance]) ||
			    !PAGE_ALIGNED(params->mmio_size))
				return PKVM_TEGRA_DIAG_INIT_MMIO_ALIGN -
				       (int)(i * PKVM_TEGRA_SMMU_MAX_INSTANCES + instance);
			ret = pkvm_host_donate_hyp_mmio(pfn, pages, PAGE_HYP_DEVICE);
			if (ret)
				return PKVM_TEGRA_DIAG_INIT_MMIO_DONATE -
				       (int)(i * PKVM_TEGRA_SMMU_MAX_INSTANCES + instance);
			smmu->base[instance] =
				hyp_phys_to_virt(params->mmio_addr[instance]);
		}
		ret = tegra_reset_smmu(smmu);
		if (ret)
			return PKVM_TEGRA_DIAG_INIT_RESET - (int)i;
	}

	tegra_identity_mm_ops = (struct kvm_pgtable_mm_ops) {
		.zalloc_page = tegra_atomic_zalloc_page,
		.zalloc_pages_exact = tegra_atomic_zalloc_pages_exact,
		.free_pages_exact = tegra_atomic_free_pages_exact,
		.free_unlinked_table = tegra_atomic_free_unlinked,
		.get_page = kvm_iommu_get_page_atomic,
		.put_page = kvm_iommu_reclaim_pages_atomic,
		.page_count = tegra_page_count,
		.phys_to_virt = hyp_phys_to_virt,
		.virt_to_phys = hyp_virt_to_phys,
	};
	tegra_domain_mm_ops = (struct kvm_pgtable_mm_ops) {
		.zalloc_page = tegra_domain_zalloc_page,
		.zalloc_pages_exact = tegra_domain_zalloc_pages_exact,
		.free_pages_exact = tegra_domain_free_pages_exact,
		.free_unlinked_table = tegra_domain_free_unlinked,
		.get_page = kvm_iommu_get_page,
		.put_page = kvm_iommu_put_page,
		.page_count = tegra_page_count,
		.phys_to_virt = hyp_phys_to_virt,
		.virt_to_phys = hyp_virt_to_phys,
	};
	memset(&tegra_identity_domain, 0, sizeof(tegra_identity_domain));
	tegra_identity_domain.cb = 0;
	tegra_identity_domain.vmid = 0;
	hyp_spin_lock_init(&tegra_identity_domain.lock);
	ret = tegra_init_pgtable(&tegra_identity_domain,
				 &tegra_identity_mm_ops,
				 address_bits, true);
	if (ret)
		return PKVM_TEGRA_DIAG_INIT_PGTABLE;
	for (i = 0; i < pkvm_tegra_smmu_count; i++)
		tegra_program_context(&tegra_smmus[i], &tegra_identity_domain);

	ret = kvm_iommu_register_pviommu_drv(driver_id);
	return ret ? PKVM_TEGRA_DIAG_INIT_PVIOMMU : 0;
}

struct kvm_iommu_ops pkvm_tegra_smmu_ops = {
	.init = tegra_init,
	.init_devices = tegra_init_devices,
	.host_stage2_snapshot_start = tegra_snapshot_start,
	.host_stage2_snapshot_end = tegra_snapshot_end,
	.host_stage2_idmap = tegra_host_stage2_idmap,
	.alloc_domain = tegra_alloc_domain,
	.free_domain = tegra_free_domain,
	.attach_dev = tegra_attach_dev,
	.detach_dev = tegra_detach_dev,
	.map_pages = tegra_map_pages,
	.unmap_pages = tegra_unmap_pages,
	.iova_to_phys = tegra_iova_to_phys,
	.set_identity = tegra_set_identity,
	.dev_block_dma = tegra_dev_block_dma,
	.get_iommu_token_by_id = tegra_iommu_token,
};
