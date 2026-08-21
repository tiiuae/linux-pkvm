.. SPDX-License-Identifier: GPL-2.0

==========================
Ghaf Orin pKVM Provenance
==========================

This document describes the immutable ``orin-pkvm-v55`` source generation.
The machine-readable source of truth is ``pkvm-provenance.yaml`` in this
directory.  Update both files in one commit whenever a dependency changes.

Layer Contract
==============

``base/v7.1.7`` points directly at Linux stable v7.1.7.  The
``port/android17-pkvm-v7.1.7-r1`` branch contains only Android-derived pKVM
device-assignment work.  The ``integration/orin-pkvm-v7.1.7-r1`` branch has
the port head as its exact first parent and adds only Ghaf/Orin integration,
diagnostics, and this provenance record.

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
     - Source/config/build parity

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
* Rewritten Ghaf head: ``7acaec4eafe5768218e3b23a1fd68aa8a2613ca5``
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
new-image flash.  The rebuilt image SHA-256 is
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
``7acaec4eafe5`` is the validated four-commit consumer rewrite; its old
``6bdd5eaa903c`` head remains available under the archive ref recorded in the
manifest.
