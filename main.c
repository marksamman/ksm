/*
 * ksm - a really simple and fast x64 hypervisor
 * Copyright (C) 2016 Ahmed Samy <f.fallen45@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <ntddk.h>
#include <intrin.h>

#include "ksm.h"
#include "dpc.h"
#include "pe.h"

#ifdef ENABLE_ACPI
static DEV_EXT g_dev_ext = { NULL, NULL };
#endif

/*
 * Main entry point, calls ksm_init() to virtualize the system, on failure,
 * an error is printed, DebugView can be used to see the error if compiled
 * with debug.
 */
#ifndef __GNUC__
DRIVER_INITIALIZE DriverEntry;
#pragma alloc_text(INIT, DriverEntry)
#endif

PLIST_ENTRY PsLoadedModuleList;
void *g_kernel_base = NULL;
uintptr_t g_driver_base;
uintptr_t g_driver_size;

uintptr_t pxe_base = 0xfffff6fb7dbed000ull;
uintptr_t ppe_base = 0xfffff6fb7da00000ull;
uintptr_t pde_base = 0xfffff6fb40000000ull;
uintptr_t pte_base = 0xfffff68000000000ull;

static void DriverUnload(PDRIVER_OBJECT driverObject)
{
	UNREFERENCED_PARAMETER(driverObject);
#ifdef ENABLE_ACPI
	deregister_power_callback(&g_dev_ext);
#endif
	VCPU_DEBUG("ret: 0x%08X\n", ksm_exit());
#ifdef DBG
	print_exit();
#endif
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
#ifdef DBG
	/* Stupid printing interface  */
	if (!NT_SUCCESS(print_init())) {
		DbgPrint("failed to initialize log!\n");
		return STATUS_ABANDONED;
	}
#endif

	/* On Windows 10 build 14316+ Page table base addresses are not static.  */
	RTL_OSVERSIONINFOW osv;
	osv.dwOSVersionInfoSize = sizeof(osv);

	NTSTATUS status = RtlGetVersion(&osv);
	if (!NT_SUCCESS(status))
		return status;

	LDR_DATA_TABLE_ENTRY *entry = driverObject->DriverSection;
	PsLoadedModuleList = entry->InLoadOrderLinks.Flink;
	driverObject->DriverUnload = DriverUnload;

	if (osv.dwMajorVersion >= 10 && osv.dwBuildNumber >= 14316) {
		static const u8 pattern[] = {
			0x48, 0x8b, 0x04, 0xd0,  // mov     rax, [rax+rdx*8]
			0x48, 0xc1, 0xe0, 0x19,  // shl     rax, 19h
			0x48, 0xba,              // mov     rdx, ????????`????????  ; PTE_BASE
		};

		u8 *base = (u8 *)MmGetVirtualForPhysical;
		bool found = false;
		for (size_t i = 0; i <= 0x50 - sizeof(pattern); ++i) {
			if (RtlCompareMemory(pattern, &base[i], sizeof(pattern)) == sizeof(pattern)) {
				pte_base = *(uintptr_t *)(base + i + sizeof(pattern));

				uintptr_t idx = (pte_base >> PXI_SHIFT) & PTX_MASK;
				pde_base = pte_base | (idx << PPI_SHIFT);
				ppe_base = pde_base | (idx << PDI_SHIFT);
				pxe_base = ppe_base | (idx << PTI_SHIFT);
				found = true;
				break;
			}
		}

		if (!found)
			return STATUS_NOT_FOUND;

		uintptr_t tmp = (uintptr_t)PAGE_ALIGN((uintptr_t)MmGetVirtualForPhysical);
		VCPU_DEBUG("PXE: %p PPE %p PDE %p PTE %p\n", pxe_base, ppe_base, pde_base, pte_base);
		VCPU_DEBUG("Addr 0x%X 0x%X\n", __pa((uintptr_t *)tmp), va_to_pa(tmp));
	}

	VCPU_DEBUG("We're mapped at %p (size: %d bytes (%d KB), on %d pages)\n",
		   entry->DllBase, entry->SizeOfImage, entry->SizeOfImage / 1024, entry->SizeOfImage / PAGE_SIZE);
	g_driver_base = (uintptr_t)entry->DllBase;
	g_driver_size = entry->SizeOfImage;

	LDR_DATA_TABLE_ENTRY *kentry = container_of(PsLoadedModuleList->Flink, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
	g_kernel_base = kentry->DllBase;

	VCPU_DEBUG("Kernel: %p -> %p (size: 0x%X pages: %d) path: %wS\n",
		   kentry->DllBase, (uintptr_t)kentry->DllBase + kentry->SizeOfImage,
		   kentry->SizeOfImage, BYTES_TO_PAGES(kentry->SizeOfImage),
		   kentry->FullDllName.Buffer);
#ifndef __GNUC__
	ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
#endif

	if (!NT_SUCCESS(status = ksm_init()))
		goto out;

#ifdef ENABLE_ACPI
	if (NT_SUCCESS(status = register_power_callback(&g_dev_ext)))
		goto out;
#endif

#ifdef ENABLE_ACPI
	ksm_exit();
#endif
out:
	VCPU_DEBUG("ret: 0x%08X\n", status);
	return status;
}
