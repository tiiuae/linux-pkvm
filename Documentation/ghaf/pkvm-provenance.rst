.. SPDX-License-Identifier: GPL-2.0

==========================
Ghaf Orin pKVM Provenance
==========================

This document describes the immutable ``orin-pkvm-v55`` source generation,
the validated protected-PCI WLAN R2 follow-up, and the protected accelerated
GUIVM R3 development generation.  The machine-readable source of truth is
``pkvm-provenance.yaml`` in this directory.  Update both files in one commit
whenever a dependency changes.

Layer Contract
==============

``base/v7.1.7`` points directly at Linux stable v7.1.7.  The
``port/android17-pkvm-v7.1.7-r1`` branch contains only Android-derived pKVM
device-assignment work.  The ``integration/orin-pkvm-v7.1.7-r1`` branch has
the port head as its exact first parent and adds only Ghaf/Orin integration,
diagnostics, and this provenance record.  The
``integration/orin-pkvm-v7.1.7-r2`` branch starts at the immutable R1 head and
adds the protected-PCI WLAN series without changing the v55 tag.  The
``integration/orin-pkvm-v7.1.7-r3`` branch starts at the validated R2 head and
adds the protected GPU/display reset contract.  R3 does not move either the
v55 tag or the R2 boundary.

Validated branches are immutable.  A new Linux stable base creates a new
``v7.1.x-rN`` port and integration pair instead of rewriting this generation.

Dependency Table
================

The ``ID`` values below are stable keys shared with the YAML manifest.  The
``External`` column identifies code owned outside Ghaf.  ``Port/Adaptation``
identifies the intervening commit or series in this repository.  ``Internal``
identifies Orin-specific code or the Ghaf consumer.  ``Gate`` identifies the
minimum evidence that must be refreshed when the row changes.

.. list-table:: External, Port, And Internal Dependencies
   :header-rows: 1
   :widths: 12 21 25 25 17

   * - ID
     - External
     - Port/Adaptation
     - Internal
     - Gate
   * - linux-base
     - Linux stable v7.1.7, ``c7ba9d6de43e``
     - Exact base ref; no local commit
     - All port and integration commits
     - Tag and tree identity
   * - android-alloc
     - Android pKVM allocator commits
     - ``ebf1bc0143fe``
     - Protected IOMMU allocation and refill
     - Port tree reconstruction
   * - android-iommu
     - Android IOMMU domain and hypercall commits
     - ``20291c7f73ef``
     - Tegra EL2 IOMMU backend, patches 0002-0041
     - Host and guest kernel builds
   * - android-device
     - Android protected-device lifecycle commits
     - ``437045f64dff``
     - MGBE reset, activation, teardown
     - Active-DMA teardown
   * - android-pviommu
     - Android pvIOMMU protocol commits
     - ``e2b4614bd0c4``
     - Raw SID export and protected guest attach
     - NetVM attach and lifecycle
   * - android-guest
     - Android pvIOMMU guest driver commits
     - ``2d5cb9d361b6``
     - NetVM software IOVA mirror
     - Bidirectional MGBE soak
   * - android-vfio
     - Android KVM/VFIO routing commits
     - ``c5d77d0885a3``
     - VFIO platform assignment for MGBE0
     - Protected NetVM boot
   * - android-maple
     - Android commit ``8e7a03dcb9f5``
     - Clean port ``ce9c31f559a9``
     - Patch 0041 accounting remains diagnostic-only
     - 8 GiB each way and 800 probes
   * - orin-smmu
     - Android port layer and Tegra234 register contract
     - Old patches 0002-0018
     - Tegra SMMUv2 EL2 ownership and host domains
     - Protected nVHE host boot
   * - orin-dma
     - Linux DMA/IOMMU APIs and Android port layer
     - Old patches 0019-0028
     - Translated/identity DMA routing and diagnostics
     - Host storage, XUSB, and MGBE
   * - orin-vfio
     - Linux VFIO/KVM and Android device lifecycle
     - Old patches 0029-0035, with 0032 subsumed by 0031
     - Assigned-device activation and guest DMA aperture
     - VM boot and active teardown
   * - orin-streams
     - Tegra stream-match and pKVM host pinning APIs
     - Old patches 0036-0041
     - Raw SID 6, overlapping pins, DMA accounting
     - Clean host/guest fault scans
   * - ghaf-consumer
     - This integration branch and immutable v55 tag
     - One external ``linux-pkvm`` flake input
     - Shared host/guest package set; config-only differences
     - Source/config/build/identified-AGX parity
   * - ghaf-parent
     - Ghaf PR #2133, ``21e67aebdfad``
     - Four-commit PR #2144 rebase, ``5effb128640a``
     - Protected AGX target and service plane
     - Range-diff, build, flash, and runtime parity
   * - jetpack-pr22
     - ``tiiuae/jetpack-nixos#22``, ``9ad43a2697bc``
     - Provider-owned Linux 7.1 BPMP compatibility
     - AGX external-kernel package selection
     - Target check, full image, and AGX boot
   * - microvm-pr586
     - ``microvm-nix/microvm.nix#586``, ``0299f2d5faf5``
     - Provider-owned protected-VM and platform interfaces
     - Ghaf topology and target policy
     - Provider and Ghaf platform checks
   * - ghaf-crosvm-pr10
     - ``tiiuae/ghaf-crosvm#10``, merged ``63e3c3482553``
     - Provider-owned UAS and xHCI fixes; reviewed head ``f2f3d2102da1``
     - Combined Crosvm source baseline; protected VMs keep USB disabled
     - Crosvm builds and full-image regression gate
   * - ghaf-crosvm-pr12
     - ``tiiuae/ghaf-crosvm#12``, ``aa2478bef075``
     - No-IOMMU-only extra VFIO map; KVM memslot in all modes
     - Protected MGBE platform assignment
     - Crosvm builds, MGBE soak, and lifecycle
   * - ghaf-crosvm-create-vm
     - Crosvm source at ``aa2478bef075``
     - Ghaf protected-create-VM compatibility patch
     - Protected AdminVM, NetVM, and ChromiumVM (R1/R2) or GUIVM (R3)
     - Target check and three active protected VMs
   * - ghaf-tfa
     - TF-A source selected through Jetpack/Ghaf
     - Ghaf target-local protected-host firmware patch
     - AGX protected-host firmware integration
     - Full image, recovery flash, protected nVHE
   * - ghaf-mgbe-dt
     - Tegra234 DT and ``orin-pkvm-v55`` interfaces
     - Ghaf target-local MGBE device-tree overlay
     - NetVM owns ``6800000.ethernet`` via ``pkvm-iommu``
     - SID 6, soak, teardown, and settled cycles
   * - ghaf-service-plane
     - ``microvm-pr586`` and ``orin-pkvm-v55`` interfaces
     - No generated kernel patch
     - AdminVM, NetVM, and ChromiumVM (R1/R2) or GUIVM (R3) ordering and policy
     - Three protected VMs and independent NetVM recovery
   * - kernel-pci-wlan-r2
     - Linux PCI, Tegra194 PCIe, and ``orin-pkvm-v55`` interfaces
     - Eight commits, ``8186853a2517`` through ``24e85e20b92b``
     - Protected RTL8822CE registration, reset, BAR, DMA, and MSI handling
     - Build, traffic, active teardown, cycles, and clean fault scans
   * - kernel-guivm-reset-r3
     - Linux pKVM device lifecycle, Linux Host1x, and NVIDIA L4T R36.5 GPU/display register contracts
     - ``262b04e976f7``; per-resource mapping below
     - Protected assignment of the eleven accelerated GUIVM platform resources
     - Kernel build, protected boot, accelerated display, and repeated teardown
   * - jetpack-guivm-linux71
     - NVIDIA L4T R36.5 GPU/display sources and Linux 7.1 APIs
     - ``jetpack-nixos`` commits ``6d1f6fb`` and ``ec26ef0``
     - Linux 7.1 accelerated GUIVM module closure and devfreq governor
     - Full cross image and identified-AGX unprotected GUI runtime
   * - ghaf-guivm-linux71
     - ``jetpack-guivm-linux71`` and Ghaf PR #2133/#2144/#2188 stack
     - Ghaf commit ``21f986176``
     - Intermediate unprotected accelerated GUIVM before protected composition
     - Linux 7.1.8, ``nvhost_podgov``, DP-1, greetd, and clean devfreq fault scan
   * - ghaf-device-manager-guivm-evdev
     - ``ghaf-device-manager`` merged overlay baseline ``97835a588f65``
     - Opt-in USB evdev commit ``20148e27488e``
     - Host owns ``046d:c52b`` and forwards the ``Logitech K400 Plus`` event stream to GUIVM
     - Unit tests, Clippy, REUSE, generated config, and runtime input
   * - ghaf-protected-guivm-r3
     - ``kernel-guivm-reset-r3``, ``jetpack-guivm-linux71``, and ``ghaf-device-manager-guivm-evdev``
     - Ghaf commit ``16b65edbc0b5``
     - Protected AdminVM, NetVM, and accelerated GUIVM; 11 GPU/display resources via ``pkvm-iommu``
     - Full cross image, protected boot, accelerated display/input, and repeated teardown
   * - microvm-pci-wlan
     - ``microvm-nix/microvm.nix#586`` interfaces
     - Draft PR #589, ``254dccf3f126``
     - Explicit static PCI assignment with ``pkvm-iommu``
     - Provider check, Ghaf target evaluation, and AGX lifecycle
   * - ghaf-crosvm-pci-wlan
     - ``tiiuae/ghaf-crosvm#12`` interfaces
     - Draft PR #13, ``0b9383f74be7`` through ``718c58c3606b``
     - Guest pvIOMMU map, VFIO device registration, reset-safe shutdown
     - Crosvm builds, live MSI, active teardown, and three cycles
   * - ghaf-pci-wlan-consumer
     - Exact R2 kernel, microvm, and Crosvm commits above
     - Draft Ghaf PR #2188, ``e48209099c0b``
     - Assign onboard ``10ec:c822`` to protected NetVM
     - Full image and identified-AGX Wi-Fi runtime campaign

R3 GPUVM Reset Mapping
=======================

The R3 reset commit is intentionally source-derived rather than a blanket
no-op admission rule.  Each row records the external register contract, this
repository's adaptation, and the internal resource that consumes it.  Update
this table and the matching YAML ``gui_vm_r3.reset_contract`` entries whenever
the provider manifest or reset implementation changes.

.. list-table:: External Reset Contracts And Internal GPUVM Resources
   :header-rows: 1
   :widths: 18 29 21 20 12

   * - Resources
     - External contract
     - R3 adaptation
     - Internal consumer
     - Reset class
   * - ``vm_hs_p``, ``vm_cma_p``, ``scanout_p``
     - Jetpack virtualization manifest and removed-memory overlay
     - Explicit mandatory reset callback; no register access
     - Guest heaps and host-mediated scanout buffers
     - Non-executing memory
   * - ``disp_caps_pt``, ``disp_cursor_pt``
     - NVIDIA ``NVC673`` capability page and ``NVC67A`` cursor PIO class
     - Explicit mandatory reset callback; no DMA engine
     - Read-only capabilities and immediate cursor methods
     - Read/PIO wrapper
   * - ``disp_chan_pt``
     - NVIDIA ``NVC67D`` PUT/GET DMA-control pages
     - Reclaim pages and set each PUT to its hardware GET
     - DCE-mediated core and window command channels
     - Doorbell quiesce
   * - ``17000000.gpu``
     - NVIDIA GA10B MC engine reset and CPU interrupt-mask sequence
     - Mask top interrupts, clear ``MC_DEVICE_ENABLE``, and poll reset
     - GA10B graphics and copy engines
     - Hardware reset
   * - ``13e00000.host1x_pt``
     - Linux T234 Host1x DMA stop, command stop, and channel teardown
     - Apply the sequence to all 63 Host1x channels
     - Host1x command DMA for VIC, NVDEC, and NVJPG
     - DMA teardown
   * - ``15340000.vic``, ``15540000.nvjpg``
     - NVIDIA Falcon interrupt mask, interface disable, and CPU hard reset
     - Reclaim the Falcon page and assert ``CPUCTL.HRESET``
     - VIC and NVJPG firmware processors
     - Firmware reset
   * - ``15480000.nvdec``
     - Linux T234 NVDEC RISC-V boot and boot-DMA registers
     - Clear boot-DMA configuration and the RISC-V start latch
     - NVDEC firmware processor
     - Firmware quiesce

Source Anchors
==============

* Linux repository: ``https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git``
* Linux tag: ``v7.1.7``
* Linux commit: ``c7ba9d6de43e9d9bd755b1f3c19501a38898c6b6``
* Android repository: ``https://android.googlesource.com/kernel/common``
* Android branch: ``android17-6.18``
* Android donor tip: ``e5d208fff880b1774013598e50595242fb573ac6``
* Validated old Ghaf head: ``6bdd5eaa903c2ca92a959bf0426aea18236b4726``
* Validated old Ghaf base: ``87632d714a64ddd63164f354bba9a460a2d700c4``
* Hardware-validated Ghaf consumer head: ``5effb128640a185369b678b8d150da617c8b550e``
* Flashed replay head: ``7a4b163f76b2302bea90a32696aa20280bba185d``
* Current Ghaf PR #2133 parent: ``21e67aebdfad2a6ae924844c7cfb1901cb651a09``
* Current rebased Ghaf head: ``5effb128640a185369b678b8d150da617c8b550e``
* Ghaf consumer pin: ``a62ea5215093d4595de020d5ae55e2a74d274491``

Equivalence Contract
====================

The code tree at integration commit ``a62ea5215093`` matches the union of the
43 old PR #2144 kernel patch files exactly.  The only files added after that
comparison are this document and ``pkvm-provenance.yaml``.

The old host and assigned-guest patch selections were different subsets.  The
external repository contains their union so both use one source commit;
host/guest differences remain kernel configuration.  Patch 0032 is recorded
as a dependency-only commit because its guest diagnostic hunk is identical to
the hunk already present in host patch 0031.

A disposable replay of the seven manifest port commits onto ``base/v7.1.7``
produced an empty tree difference and an all-equal ``git range-diff``.
A disposable v7.1.8 candidate replay also applied without conflicts.  Its
range-diff was all equal except for patch 0032, whose intentionally empty,
subsumed dependency commit was omitted from the candidate.

Maintenance Procedure
=====================

1. Add or update a YAML dependency row before changing source.
2. Record every external origin commit and whether the port is exact,
   adapted, multi-source, local, or subsumed.
3. Reconstruct the port from the manifest in a disposable worktree.
4. Require an empty code-tree difference against the intended oracle.
5. Rebuild host and assigned-guest kernels from the same source commit.
6. Refresh the Ghaf source/config range-diff and the validation fields.
7. For a stable update, create a new generation; never rewrite this one.

Validated v55 Boundary
======================

Both the old patch stack and the external-source rewrite built and flashed
Linux 7.1.7 to the same authorized AGX with TOPO serial ``TOPOED73D35C`` and
ECID ``0x80012344705DD3C96C0000000A0081C0``.  NX APX was absent before the
new-image flash.  The original external-source parity image SHA-256 is
``162b8c22af9bbc59d6c9d158c99d5224829c31bf0857db5bba48d71c8905ac36``.

The generated host configuration is byte-for-byte equal to the old stack.
Protected-guest differences are limited to newly visible disabled/default
symbols; all host and guest kernels use source commit ``a62ea5215093``.

Protected nVHE, AdminVM, NetVM, ChromiumVM, 8 GiB per physical-MGBE
direction, all 800 probes, corrected active-DMA teardown, and three settled
NetVM cycles passed on the new image.  AdminVM and ChromiumVM PIDs remained
unchanged during the lifecycle campaign.  Host, AdminVM, and ChromiumVM fault
scans were empty.  NetVM retained only its existing boot-time BPMP
preemption-imbalance warning; no workload or lifecycle Maple Tree, pvIOMMU,
DMA-unmap, SLUB, SMMU, watchdog, warning, or panic signature appeared.  Both
protected-DMA domains reported ``failures=0``.

Annotated tag ``orin-pkvm-v55`` points at exact code-parity commit
``a62ea5215093``.  The integration branch adds provenance documentation after
that tag; consumers pin the tagged code commit.  Ghaf PR #2144 head
``7acaec4eafe5`` is the hardware-validated four-commit consumer rewrite; its
old ``6bdd5eaa903c`` head remains available under the archive ref recorded in
the manifest.

After PR #2125 merged, it became ordinary history rather than a live
dependency.  PR #2144 now depends only on PR #2133.  The current four-commit
consumer is based on PR #2133 head ``21e67aebdfad`` and ends at
``5effb128640a``.  It retains the same immutable kernel pin and exact provider
pins for Jetpack PR #22 and microvm PR #586.  Its Crosvm source combines
merged PR #10 at ``63e3c3482553`` with PR #12 at ``aa2478bef075``.

The first canary of this parent generation exposed a Crosvm early-map bug:
pKVM rejected an extra VFIO DMA mapping before the KVM memslot was installed,
and NetVM panicked on an MGBE region read.  Ghaf-crosvm PR #12 restricts that
extra mapping to ``NoIommu`` while retaining the KVM memslot in every mode.
PR #12 was rebased without content change onto merged PR #10; its stable patch
ID remains ``398007f2cd35d2920ce7dcbb60633cf4bd16e1de``.  The accepted image
pins exact combined provider commit ``aa2478bef075``.  Its SHA-256 is
``78ef31acc5072c09ecb5e4b9ef55a4d6f12d39d8349c1965779cb978ece1b85c``.

That exact image was flashed to the identified AGX after proving NX APX was
absent.  Protected nVHE and all three protected VMs passed.  The MGBE campaign
moved 8 GiB in each direction with all 32 streams and 800 probes passing,
then passed corrected active-DMA teardown and three settled NetVM cycles.
Categorized host and guest scans found no Maple Tree, pvIOMMU, DMA-unmap,
SLUB-corruption, SMMU-fault, watchdog, panic, BUG, or Oops lines.  All 311
protected-DMA accounting samples from the previous generation and all 315
samples from the current generation reported zero failures.  The current head
``5effb128640a`` replays the same four patches onto the next
published PR #2133 head.  Its generated kernel configurations and complete
image store artifact are byte-identical to the artifact flashed from
``7a4b163f76b2``.  This makes ``5effb128640a`` the current validated consumer
boundary without a second destructive flash; the immutable kernel tag remains
on exact code-parity commit ``a62ea5215093``.

The flashed replay head is preserved as
``archive/pr2144-pre-pr2133-21e67a-7a4b163f`` and the previous remote consumer
head remains preserved as ``archive/pr2144-pre-pr2133-35e8df-2ab6784e``.  The
current range-diff has SHA-256
``05e83a01250f2a1fe8c0e5978bc21c7af82423e1fd10fe201915aa9091042fe1``.
The accepted image boots host toplevel ``8y9nrg37ss9k`` and has SHA-256
``78ef31acc5072c09ecb5e4b9ef55a4d6f12d39d8349c1965779cb978ece1b85c``.

Protected PCI WLAN R2 Boundary
================================

The R2 code boundary is ``24e85e20b92b``.  It is eight commits on top of the
documented R1 head and does not move or reinterpret ``orin-pkvm-v55``.  Ghaf
draft PR #2188 pins that exact kernel commit, ghaf-crosvm draft PR #13 at
``718c58c3606b``, and microvm.nix draft PR #589 at ``254dccf3f126``.  Its
consumer commit is ``e48209099c0b``.

The image flashed for the R2 hardware campaign is
``/nix/store/lvh3d0l3fcy4bqxillm5r2s8kfpmn8l6-nixos-image-sd-card-26.11.20260819.ffb3c9b-aarch64-linux.img.zst-aarch64-unknown-linux-gnu``.
It is 8,378,722,232 bytes with SHA-256
``c6483a71368e642b79b285f09e2237a054308a5de72f9fb0895b85eac766bc32``.
The flash used only the long ``--signed-sd-image`` option, never ``-s`` or a
secure-boot request, and reported ``Boot Authentication: NS``.  It targeted
only AGX TOPO serial ``TOPOED73D35C``, USB instance ``1-1.2.3``, and ECID
``0x80012344705DD3C96C0000000A0081C0`` after proving NX APX was absent.
Persistent ``/tmp/rcm_state`` remained unchanged.

Protected nVHE and AdminVM, NetVM, and ChromiumVM all passed.  NetVM attached
physical ``0001:01:00.0`` as guest ``0000:00:1f.0``, identified
``10ec:c822``, loaded the RTL8822CE firmware, and used MSI IRQ 35.  Association
to the requested WLAN, Wi-Fi-bound ICMP, DNS, and HTTPS all passed.  Active
Wi-Fi teardown returned service success, and three complete stop/start cycles
each prepared the endpoint in D0 with memory decoding enabled and bus
mastering cleared before the mandatory pKVM reset.  The final cycle passed
10/10 probes and HTTPS status 200.  AdminVM and ChromiumVM remained unchanged
through the campaign.

The final host and guest scans contained no Maple Tree, pvIOMMU failure,
SLUB, SMMU fault, AER unsupported request, watchdog, lockup, RCU stall, BUG,
or panic signature.  Protected-DMA diagnostics retained ``failures=0``.
Crosvm still emits closed-channel and dynamic-mapping cleanup messages during
orderly shutdown; the units nevertheless exit successfully, so those messages
are recorded as non-fatal cleanup behavior rather than omitted from evidence.
The retained flash and serial records are
``/home/vadikas/Work/tmp-archive/pkvm-wifi-d0-reset-flash.oFSxAd/flash.log``
and
``/home/vadikas/Work/tmp-archive/pkvm-wifi-d0-reset-runtime.gRlinq/serial.log``.

An independent build from only the published immutable pins also passed.  Its
image is
``/nix/store/pldypg6g14298i33x5ymmqlv2vxmn2i3-nixos-image-sd-card-26.11.20260819.ffb3c9b-aarch64-linux.img.zst-aarch64-unknown-linux-gnu``;
the compressed file is 8,379,136,200 bytes with SHA-256
``eb71c6e37e22f80fb7da6fff20ea9d2811ed5455990539542f54ba6b62422099``.
It is a separate Nix realization from the locally sourced flashed artifact,
not a byte-identity claim.  The kernel, Crosvm, and microvm source trees used
for the runtime campaign are exactly the trees published at the pinned
commits above.
