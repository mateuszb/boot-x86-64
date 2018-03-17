
/* (C) 2014 Mateusz Berezecki. All rights reserved. */
#include <Uefi.h>
#include <Uefi/UefiSpec.h>
#include <Library/PcdLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/LoadFile.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/SimpleFileSystem.h>

#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/EdidOverride.h>
#include <Protocol/GraphicsOutput.h>

#include <Guid/MemoryTypeInformation.h>
#include <Guid/Acpi.h>
#include <Guid/Mps.h>
#include <Guid/SmBios.h>

#include "elf64.h"
#include "handoff.h"

static EFI_HANDLE gImageHandle;
static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS; 
static EFI_RUNTIME_SERVICES *gRS;
static EFI_PHYSICAL_ADDRESS gFrameBuffer = 0;
static UINTN gFrameBufferSize = 0;
static UINT32 gPixelsPerScanLine = 0;
static UINT32 gPixelSize = 0;
static UINT32 gXres = 0;
static UINT32 gYres = 0;

struct handoff_block hob;

void configure_display();

#ifdef _M_IX86
static unsigned int entry_point;
#else
static unsigned long long entry_point;
#endif

void *next_entry(void *ptr, unsigned int size)
{
	char *p = (char *)ptr;
	p += size;

	return (void *)p;
}

void *boot_memcpy(void *dst, void *src, unsigned long long size)
{
	unsigned long long i;
	char *p, *q;

	if (!dst || !src)
		return dst;

	p = (char *)dst;
	q = (char *)src;

	for (i = 0; i < size; i++, p++, q++) {
		*p = *q;
	}

	return dst;
}

int boot_memcmp(void *a, void *b, unsigned long long size)
{
	unsigned long long i;
	char *p, *q;
	
	if (!a && !b)
		return 0;
	if (!a)
		return -1;
	if (!b)
		return 1;

	p = (char *)a;
	q = (char *)b;

	for (i = 0; i < size; i++, p++, q++) {
		if (*p != *q)
			return *p - *q;
	}

	return 0;
}

void *boot_memset(void *dst, char val, unsigned long long size)
{
	unsigned long long k;
	char *p = (char *)dst;

	if (!dst)
		return dst;
	for (k = 0; k < size; k++, p++)
		*p = val;

	return dst;
}

void locate_rsdp(EFI_SYSTEM_TABLE *systab)
{
	EFI_CONFIGURATION_TABLE *conf = systab->ConfigurationTable;
	UINTN i;
	EFI_GUID acpi_guid = EFI_ACPI_TABLE_GUID;
	EFI_GUID smbios_guid = SMBIOS_TABLE_GUID;
	EFI_GUID mps_guid = MPS_TABLE_GUID;

	for (i = 0; i < systab->NumberOfTableEntries; i++) {
		if (conf[i].VendorGuid.Data1 == acpi_guid.Data1 &&
			conf[i].VendorGuid.Data2 == acpi_guid.Data2 &&
			conf[i].VendorGuid.Data3 == acpi_guid.Data3) {
			hob.rsdp = conf[i].VendorTable;
		}

		if (conf[i].VendorGuid.Data1 == smbios_guid.Data1 &&
			conf[i].VendorGuid.Data2 == smbios_guid.Data2 &&
			conf[i].VendorGuid.Data3 == smbios_guid.Data3) {
			hob.smbios = conf[i].VendorTable;
		}

		if (conf[i].VendorGuid.Data1 == mps_guid.Data1 &&
			conf[i].VendorGuid.Data2 == mps_guid.Data2 &&
			conf[i].VendorGuid.Data3 == mps_guid.Data3) {
			hob.mps = conf[i].VendorTable;
		}
	}
}

int prepare_elf(void *kbuf)
{
	struct elf64_ehdr *ehdr;
	struct elf64_shdr *shdr;
	struct elf64_phdr *phdr;
	unsigned int k, m;

	ehdr = (struct elf64_ehdr *)kbuf;
	entry_point = (unsigned int)ehdr->e_entry;

	Print(L"EFI: kernel entry point %X\n", ehdr->e_entry);
	Print(L"EFI: kernel program header offset %X\n", ehdr->e_phoff);
	Print(L"EFI: kernel section header offset %X\n", ehdr->e_shoff);
	Print(L"EFI: kernel ELF image has %d sections of size %d.\n",
		ehdr->e_shnum, ehdr->e_shentsize);
	Print(L"EFI: kernel ELF image has %d segments of size %d.\n",
		ehdr->e_phnum, ehdr->e_phentsize);

	if (ehdr->e_type != ET_EXEC) {
		Print(L"EFI: kernel ELF file is not of ET_EXEC type.\n");
		return -1;
	}

	/* Configure section and program header offsets
		based on the ELF header */
	shdr = (struct elf64_shdr *)((char *)kbuf + ehdr->e_shoff);
	phdr = (struct elf64_phdr *)((char *)kbuf + ehdr->e_phoff);
	
	for (k = 0; k < ehdr->e_shnum; k++) {
		unsigned long long pstart;

		phdr = (struct elf64_phdr *)((char *)kbuf + ehdr->e_phoff);
		pstart = 0;
		for (m = 0; m < ehdr->e_phnum; m++) {
			if ((phdr->p_vaddr <= shdr->sh_addr) && ((phdr->p_vaddr + phdr->p_memsz) >= shdr->sh_addr)) {
				pstart = phdr->p_paddr + (shdr->sh_addr - phdr->p_vaddr);
				break;
			}

			phdr = (struct elf64_phdr *)next_entry(phdr, ehdr->e_phentsize);
		}

		
		if (phdr->p_type == PT_LOAD && phdr->p_filesz) {
			if (shdr->sh_type == SHT_NOBITS && shdr->sh_size) {
				if (shdr->sh_flags & SHF_ALLOC) {
					Print(L"EFI: Zero loading section %llX-%llX at %llX\n",
						shdr->sh_addr, shdr->sh_addr + shdr->sh_size, pstart);
				}
			} else if (shdr->sh_type == SHT_PROGBITS && shdr->sh_size) {
				if (shdr->sh_flags & SHF_ALLOC) {
					Print(L"EFI: Loading section %llX-%llX at %llX\n",
						shdr->sh_addr, shdr->sh_addr + shdr->sh_size, pstart);
				}
			}
		}
		
		shdr = (struct elf64_shdr *)next_entry(shdr, ehdr->e_shentsize);
	}

	return 0;
}

void start_elf(void *kbuf)
{	
	struct elf64_ehdr *ehdr;
	struct elf64_shdr *shdr;
	struct elf64_phdr *phdr;
	unsigned int k, m;

	void (*kmain)(void *) = (void (*)(void *))entry_point;
	ehdr = (struct elf64_ehdr *)kbuf;
	shdr = (struct elf64_shdr *)((char *)kbuf + ehdr->e_shoff);
	phdr = (struct elf64_phdr *)((char *)kbuf + ehdr->e_phoff);

	/* Walk ELF file program sections */
	for (k = 0; k < ehdr->e_phnum; k++) {
		void *src = (void *)((char *)kbuf + phdr->p_offset);

		if (phdr->p_type == PT_LOAD && phdr->p_filesz) /* Load this section from file */
#ifdef _M_IX86
			boot_memcpy((void *)(unsigned int)phdr->p_paddr, src, phdr->p_filesz);
#else
			boot_memcpy((void *)phdr->p_paddr, src, phdr->p_filesz);
#endif
		
		phdr = (struct elf64_phdr *)next_entry(phdr, ehdr->e_phentsize);
	}

	/* Skip the first entry. It's meant to be unused according to the spec. */
	shdr = (struct elf64_shdr *)next_entry(shdr, ehdr->e_shentsize);

	for (k = 0; k < ehdr->e_shnum; k++) {
		unsigned long long pstart;
		phdr = (struct elf64_phdr *)((char *)kbuf + ehdr->e_phoff);

		pstart = 0;
		for (m = 0; m < ehdr->e_phnum; m++) {
			if ((phdr->p_vaddr <= shdr->sh_addr) && ((phdr->p_vaddr + phdr->p_memsz) >= shdr->sh_addr)) {
				pstart = phdr->p_paddr + (shdr->sh_addr - phdr->p_vaddr);
				break;
			}

			phdr = (struct elf64_phdr *)next_entry(phdr, ehdr->e_phentsize);
		}

		if (phdr->p_type == PT_LOAD && phdr->p_filesz) {
			if (shdr->sh_type == SHT_NOBITS && shdr->sh_size) {
				if (shdr->sh_flags & SHF_ALLOC) {
					boot_memset((void *)pstart, 0, shdr->sh_size);
				}
			} else if (shdr->sh_type == SHT_PROGBITS && shdr->sh_size) {
				if (shdr->sh_flags & SHF_ALLOC) {
					boot_memcpy((void *)pstart, ((char *)kbuf + shdr->sh_offset), shdr->sh_size);
				}
			}
		}
		shdr = (struct elf64_shdr *)next_entry(shdr, ehdr->e_shentsize);
	}

	/* Prepare AP boot code but don't run it. Waking up APs requires a working APIC on BSP.
	   We prepare the AP code that is loaded from the \\System\\ap_boot.bin
	   and we copy it to a <1MB space that's passed in the SIPI interrupt to all APs. */

	kmain(&hob);
}

void configure_display()
{
	EFI_EDID_ACTIVE_PROTOCOL *edidActive = NULL;
	EFI_EDID_OVERRIDE_PROTOCOL *edidOverride = NULL;
	EFI_EDID_DISCOVERED_PROTOCOL *edidDiscovered = NULL;

	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
	EFI_STATUS status;

	status = gBS->LocateProtocol(&gEfiEdidActiveProtocolGuid, NULL, (void**)&edidActive);
	if (EFI_ERROR(status)) {
		Print(L"EFI: couldn't locate EFI_EDID_ACTIVE_PROTOCOL.\n");
	}

	status = gBS->LocateProtocol(&gEfiEdidOverrideProtocolGuid, NULL, (void**)&edidOverride);
	if (EFI_ERROR(status)) {
		Print(L"EFI: Couldn't locate EDID_OVERRIDE_PROTOCOL\n");
	}

	status = gBS->LocateProtocol(&gEfiEdidDiscoveredProtocolGuid, NULL, (void**)&edidDiscovered);
	if (EFI_ERROR(status)) {
		Print(L"EFI: Couldn't find EFI_EDID_DISCOVERED_PROTOCOL\n");
	}

	status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (void**)&gop);
	if (EFI_ERROR(status)) {
		Print(L"EFI: Couldn't locate GRAPHICS_OUTPUT_PROTOCOL\n");
	} else {
		UINT32 maxMode, mode;
		UINT32 i;
		EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *gopMode = gop->Mode;
		//EFI_PIXEL_BITMASK *pixelMask;
		EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *modeInfo;

		UINT16 *pxlFmt[] = {
			L"PixelRedGreenBlueReserved8BitPerColor",
			L"PixelBlueGreenRedReserved8BitPerColor",
			L"PixelBitMask",
			L"PixelBltOnly"
		};

		mode = gopMode->Mode;
		maxMode = gopMode->MaxMode;

		Print(L"EFI: GOP maxmode=%d, mode=%d\n", maxMode, mode);

		for (i = 0; i < maxMode; i++) {
			EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *tmpInfo;
			UINTN tmpSize;
			gop->QueryMode(gop, i, &tmpSize, &tmpInfo);
			Print(L"EFI: GOP mode=%d, hres=%d, vres=%d, format=%d\n",
				i, tmpInfo->HorizontalResolution, tmpInfo->VerticalResolution, tmpInfo->PixelFormat);
		}

		gop->SetMode(gop, 0);

		gopMode = gop->Mode;
		modeInfo = gopMode->Info;
		//pixelMask = &modeInfo->PixelInformation;

		Print(L"EFI: resolution: %d x %d with %d pixels per scan line.\n",
			modeInfo->HorizontalResolution,
			modeInfo->VerticalResolution,
			modeInfo->PixelsPerScanLine);

		Print(L"EFI: pixel format: %d (%s)\n", modeInfo->PixelFormat, pxlFmt[modeInfo->PixelFormat]);
		Print(L"EFI: linear frame buffer at %x\n", gopMode->FrameBufferBase);

		gXres = modeInfo->HorizontalResolution;
		gYres = modeInfo->VerticalResolution;

		gFrameBuffer = gopMode->FrameBufferBase;
		gFrameBufferSize = gopMode->FrameBufferSize;

		gPixelsPerScanLine = modeInfo->PixelsPerScanLine;
		gPixelSize = sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
	}


	if (!gFrameBuffer || !gFrameBufferSize)
		Print(L"EFI: Frame buffer configuration/detection error.\n");
}

/**
  The user Entry Point for Application. The user code starts with this function
  as the real entry point for the application.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.  
  @param[in] SystemTable    A pointer to the EFI System Table.
  
  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

**/
EFI_STATUS
EFIAPI
BootMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
	EFI_STATUS status;
	EFI_LOADED_IMAGE_PROTOCOL *image;
	EFI_DEVICE_PATH_PROTOCOL *path;
	EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *printer;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_FILE_PROTOCOL *root, *file;
	EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
	UINTN kbuf_size;
	UINTN desc_size, map_key, map_size;
	UINT32 desc_ver;
	UINTN npages;
	int once = 1;
	UINT32 i;
	void *kernel_buffer;

	gImageHandle = ImageHandle;
	gST = SystemTable;
	gBS = gST->BootServices;
	gRS = gST->RuntimeServices;

	/* Prepare screen for printing diagnostic information */
	//configure_console();
	configure_display();
	
	/* Figure out where we're loading from */
	status = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (void**)&image);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error locating LoadedImageProtocol for the bootloader image.\n");
		return status;
	}

	status = gBS->HandleProtocol(image->DeviceHandle, &gEfiDevicePathProtocolGuid, (void**)&path);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error locating DevicePathProtocol for the bootloader image.\n");
		while (status) {
#if defined _MSC_VER
			__halt();
#else
			asm("hlt");
#endif
		}
		return status;
	}

	status = gBS->LocateProtocol(&gEfiDevicePathToTextProtocolGuid, NULL, (void**)&printer);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error locating DevicePathToText protocol.\n");
		while (status) {
#if defined_MSC_VER
			__halt();
#else
			asm("hlt");
#endif
		}
		return status;
	}

	Print(L"EFI: Image device: %s\n", printer->ConvertDevicePathToText(path, FALSE, FALSE));
	Print(L"EFI: Image file: %s\n", printer->ConvertDevicePathToText(image->FilePath, FALSE, FALSE));
	Print(L"EFI: Image base: %X\n", image->ImageBase);
	Print(L"EFI: Image size: %X\n", image->ImageSize);

	/* Load the kernel file */
	status = gBS->HandleProtocol(image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void**)&fs);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error opening FS protocol.\n");
		while (status) {
#if defined _MSC_VER
			__halt();
#else
			asm("hlt");
#endif
		}
		return status;
	}

	status = fs->OpenVolume(fs, &root);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error opening volume.\n");
		while (status) {
#if defined _MSC_VER
			__halt();
#else
			asm("hlt");
#endif
		}
		return status;
	}

	status = root->Open(root, &file, L"\\System\\kernel.elf", EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error opening kernel file.\n");
		while (status) {
#if defined _MSC_VER
			__halt();
#else
			asm("hlt");
#endif
		}
		return status;
	}

	kbuf_size = 0x1000000;
	status = gBS->AllocatePool(EfiLoaderCode, kbuf_size, &kernel_buffer);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error allocating pool to hold kernel file.\n");
		while (status) {
#if defined _MSC_VER
			__halt();
#else
			asm("hlt");
#endif
		}
		return status;
	}

	status = file->Read(file, &kbuf_size, kernel_buffer);
	if (EFI_ERROR(status)) {
		Print(L"EFI: error reading kernel file into buffer.\n");
		while (status) {
#if defined _MSC_VER
			__halt();
#else
			asm("hlt");
#endif
		}
	}

	file->Close(file);
	root->Close(root);

	/* Parse kernel ELF image */
	prepare_elf(kernel_buffer);

	locate_rsdp(gST);
	hob.system_tab = gST;
	hob.boot_svc = gBS;
	hob.rt_svc = gRS;

	/* Get the memory map */
	map_size = 0;
	memory_map = NULL;
	npages = 0;

	for (i = 0; i < 10; i++) {
		if (memory_map) {
			EFI_PHYSICAL_ADDRESS addr;

			addr = (EFI_PHYSICAL_ADDRESS)memory_map;
			gBS->FreePages(addr, npages);
			map_size = 0;
			memory_map = NULL;
		}

		status = gBS->GetMemoryMap(&map_size, memory_map, &map_key, &desc_size, &desc_ver);
		if (status == EFI_BUFFER_TOO_SMALL) {
			EFI_PHYSICAL_ADDRESS addr = 0;

			if (npages == 0)
				npages = EFI_SIZE_TO_PAGES(map_size) + 1;
			else
				npages++;
			
			map_size = EFI_PAGES_TO_SIZE(npages);

			status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, npages, &addr);

#ifdef _M_IX86
			memory_map = (EFI_MEMORY_DESCRIPTOR *)(addr & 0xffffffff);
#else
			memory_map = (EFI_MEMORY_DESCRIPTOR *)addr;
#endif
		}

		status = gBS->GetMemoryMap(&map_size, memory_map, &map_key, &desc_size, &desc_ver);
		if (!EFI_ERROR(status)) {
			if (!once) {
				UINTN count = map_size / desc_size;
				UINTN ent;
				EFI_MEMORY_DESCRIPTOR *descriptor;

				descriptor = (EFI_MEMORY_DESCRIPTOR *)memory_map;
				for (ent = 0; ent < count; ent++) {
					Print(L"EFI: memory map entry %d: type=%X, phys=%X, virt=%X, npages=%X, attr=%X\n",
						ent, descriptor->Type, descriptor->PhysicalStart, descriptor->VirtualStart,
						descriptor->NumberOfPages, descriptor->Attribute);

					descriptor = next_entry(descriptor, (unsigned int)desc_size);
				}

				once = 1;
			}

			hob.memory_map = memory_map;
			hob.memory_map_desc_size = (unsigned int)desc_size;
			hob.memory_map_size = (unsigned int)map_size;

			/* No errors, so let's quit boot services now. */
			status = gBS->ExitBootServices(gImageHandle, map_key);
			if (!EFI_ERROR(status))
				break;
		} else {
			Print(L"EFI: GetMemoryMap error.\n");
		}
	}
	
	/* Once we're here, we can't call BootServices anymore. */

	/* Prepare hand-off block for the kernel */
	hob.videofb = gFrameBuffer;
	hob.videofb_size = gFrameBufferSize;
	hob.pixel_sz = gPixelSize;
	hob.pixels_per_scanline = gPixelsPerScanLine;
	hob.xres = gXres;
	hob.yres = gYres;

	if (i < 10) {
		start_elf(kernel_buffer);
	}
	while (!status) {
#if defined _MSC_VER
	      __halt();
#else
	      asm("hlt");
#endif
	}

	while (status) {
#if defined _MSC_VER
	      __halt();
#else
	      asm("hlt");
#endif
	}

	return EFI_SUCCESS;
}
