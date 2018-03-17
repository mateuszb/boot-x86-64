/* (C) 2014 Mateusz Berezecki. All rights reserved. */
#ifndef HANDOFF_H
#define HANDOFF_H

struct handoff_block {
	/* memory map information section */
	void *memory_map;
	unsigned long long memory_map_size;
	unsigned long long memory_map_desc_size;

	/* video information section */
	unsigned long long videofb;
	unsigned long long videofb_size;
	unsigned long long pixel_sz;
	unsigned long long pixels_per_scanline;
	unsigned long long xres;
	unsigned long long yres;

	void *rsdp;
	EFI_SYSTEM_TABLE *system_tab;
	EFI_BOOT_SERVICES *boot_svc;
	EFI_RUNTIME_SERVICES *rt_svc;

	void *smbios;
	void *mps;
};

#endif
