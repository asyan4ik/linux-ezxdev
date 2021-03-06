/*
 *  linux/arch/x86-64/kernel/setup.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *
 *  Nov 2001 Dave Jones <davej@suse.de>
 *  Forked from i386 setup code.
 */

/*
 * This file handles the architecture-dependent parts of initialization
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/user.h>
#include <linux/a.out.h>
#include <linux/tty.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/blk.h>
#include <linux/highmem.h>
#include <linux/bootmem.h>
#include <asm/processor.h>
#include <linux/console.h>
#include <linux/seq_file.h>
#include <asm/mtrr.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/smp.h>
#include <asm/msr.h>
#include <asm/desc.h>
#include <asm/e820.h>
#include <asm/dma.h>
#include <asm/mpspec.h>
#include <asm/mmu_context.h>
#include <asm/bootsetup.h>
#include <asm/proto.h>

/*
 * Machine setup..
 */

struct cpuinfo_x86 boot_cpu_data = { 
	cpuid_level: -1, 
};

unsigned long mmu_cr4_features;

/* For PCI or other memory-mapped resources */
unsigned long pci_mem_start = 0x10000000;

/*
 * Setup options
 */
struct drive_info_struct { char dummy[32]; } drive_info;
struct screen_info screen_info;
struct sys_desc_table_struct {
	unsigned short length;
	unsigned char table[0];
};

struct e820map e820;

unsigned char aux_device_present;

extern int root_mountflags;
extern char _text, _etext, _edata, _end;

static int disable_x86_fxsr __initdata = 0;

char command_line[COMMAND_LINE_SIZE];
char saved_command_line[COMMAND_LINE_SIZE];

struct resource standard_io_resources[] = {
	{ "dma1", 0x00, 0x1f, IORESOURCE_BUSY },
	{ "pic1", 0x20, 0x3f, IORESOURCE_BUSY },
	{ "timer", 0x40, 0x5f, IORESOURCE_BUSY },
	{ "keyboard", 0x60, 0x6f, IORESOURCE_BUSY },
	{ "dma page reg", 0x80, 0x8f, IORESOURCE_BUSY },
	{ "pic2", 0xa0, 0xbf, IORESOURCE_BUSY },
	{ "dma2", 0xc0, 0xdf, IORESOURCE_BUSY },
	{ "fpu", 0xf0, 0xff, IORESOURCE_BUSY }
};

#define STANDARD_IO_RESOURCES (sizeof(standard_io_resources)/sizeof(struct resource))

struct resource code_resource = { "Kernel code", 0x100000, 0 };
struct resource data_resource = { "Kernel data", 0, 0 };
struct resource vram_resource = { "Video RAM area", 0xa0000, 0xbffff, IORESOURCE_BUSY };


/* System ROM resources */
#define MAXROMS 6
static struct resource rom_resources[MAXROMS] = {
	{ "System ROM", 0xF0000, 0xFFFFF, IORESOURCE_BUSY },
	{ "Video ROM", 0xc0000, 0xc7fff, IORESOURCE_BUSY }
};

#define romsignature(x) (*(unsigned short *)(x) == 0xaa55)

static void __init probe_roms(void)
{
	int roms = 1;
	unsigned long base;
	unsigned char *romstart;

	request_resource(&iomem_resource, rom_resources+0);

	/* Video ROM is standard at C000:0000 - C7FF:0000, check signature */
	for (base = 0xC0000; base < 0xE0000; base += 2048) {
		romstart = bus_to_virt(base);
		if (!romsignature(romstart))
			continue;
		request_resource(&iomem_resource, rom_resources + roms);
		roms++;
		break;
	}

	/* Extension roms at C800:0000 - DFFF:0000 */
	for (base = 0xC8000; base < 0xE0000; base += 2048) {
		unsigned long length;

		romstart = bus_to_virt(base);
		if (!romsignature(romstart))
			continue;
		length = romstart[2] * 512;
		if (length) {
			unsigned int i;
			unsigned char chksum;

			chksum = 0;
			for (i = 0; i < length; i++)
				chksum += romstart[i];

			/* Good checksum? */
			if (!chksum) {
				rom_resources[roms].start = base;
				rom_resources[roms].end = base + length - 1;
				rom_resources[roms].name = "Extension ROM";
				rom_resources[roms].flags = IORESOURCE_BUSY;

				request_resource(&iomem_resource, rom_resources + roms);
				roms++;
				if (roms >= MAXROMS)
					return;
			}
		}
	}

	/* Final check for motherboard extension rom at E000:0000 */
	base = 0xE0000;
	romstart = bus_to_virt(base);

	if (romsignature(romstart)) {
		rom_resources[roms].start = base;
		rom_resources[roms].end = base + 65535;
		rom_resources[roms].name = "Extension ROM";
		rom_resources[roms].flags = IORESOURCE_BUSY;

		request_resource(&iomem_resource, rom_resources + roms);
	}
}

unsigned long start_pfn, end_pfn; 
extern unsigned long table_start, table_end;

#ifndef CONFIG_DISCONTIGMEM
static void __init contig_initmem_init(void)
{
	unsigned long bootmap_size, bootmap; 
	bootmap_size = bootmem_bootmap_pages(end_pfn)<<PAGE_SHIFT;
	bootmap = find_e820_area(0, end_pfn<<PAGE_SHIFT, bootmap_size);
	if (bootmap == -1L) 
		panic("Cannot find bootmem map of size %ld\n",bootmap_size);
	bootmap_size = init_bootmem(bootmap >> PAGE_SHIFT, end_pfn);
	e820_bootmem_free(&contig_page_data, 0, end_pfn << PAGE_SHIFT); 
	reserve_bootmem(bootmap, bootmap_size);
}
#endif

void __init setup_arch(char **cmdline_p)
{
	int i;
	unsigned long kernel_end; 

 	ROOT_DEV = to_kdev_t(ORIG_ROOT_DEV);
 	drive_info = DRIVE_INFO;
 	screen_info = SCREEN_INFO;
	aux_device_present = AUX_DEVICE_INFO;

#ifdef CONFIG_BLK_DEV_RAM
	rd_image_start = RAMDISK_FLAGS & RAMDISK_IMAGE_START_MASK;
	rd_prompt = ((RAMDISK_FLAGS & RAMDISK_PROMPT_FLAG) != 0);
	rd_doload = ((RAMDISK_FLAGS & RAMDISK_LOAD_FLAG) != 0);
#endif
	setup_memory_region();

	if (!MOUNT_ROOT_RDONLY)
		root_mountflags &= ~MS_RDONLY;
	init_mm.start_code = (unsigned long) &_text;
	init_mm.end_code = (unsigned long) &_etext;
	init_mm.end_data = (unsigned long) &_edata;
	init_mm.brk = (unsigned long) &_end;

	code_resource.start = virt_to_bus(&_text);
	code_resource.end = virt_to_bus(&_etext)-1;
	data_resource.start = virt_to_bus(&_etext);
	data_resource.end = virt_to_bus(&_edata)-1;

	parse_mem_cmdline(cmdline_p);

	e820_end_of_ram();

	init_memory_mapping(); 

#ifdef CONFIG_BLK_DEV_INITRD
	if (LOADER_TYPE && INITRD_START) {
		if (INITRD_START + INITRD_SIZE <= (end_pfn << PAGE_SHIFT)) {
			initrd_start =
				INITRD_START ? INITRD_START + PAGE_OFFSET : 0;
			initrd_end = initrd_start+INITRD_SIZE;
	}
		else {
			printk(KERN_ERR "initrd extends beyond end of memory "
			    "(0x%08lx > 0x%08lx)\ndisabling initrd\n",
			    (unsigned long)INITRD_START + INITRD_SIZE,
			    (unsigned long)(end_pfn << PAGE_SHIFT));
			initrd_start = 0;
	}
	}
#endif

#ifdef CONFIG_DISCONTIGMEM
	numa_initmem_init(0, end_pfn); 	
#else
	contig_initmem_init(); 
#endif	

	/* Reserve direct mapping */
	reserve_bootmem_generic(table_start << PAGE_SHIFT, 
				(table_end - table_start) << PAGE_SHIFT);

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start) 
		reserve_bootmem_generic(INITRD_START, INITRD_SIZE);
#endif

	/* Reserve BIOS data page. Some things still need it */
	reserve_bootmem_generic(0, PAGE_SIZE);

#ifdef CONFIG_SMP
	/*
	 * But first pinch a few for the stack/trampoline stuff
	 * FIXME: Don't need the extra page at 4K, but need to fix
	 * trampoline before removing it. (see the GDT stuff)
	 */
	reserve_bootmem_generic(PAGE_SIZE, PAGE_SIZE); 

	/* Reserve SMP trampoline */
	reserve_bootmem_generic(0x6000, PAGE_SIZE);
#endif
	/* Reserve Kernel */
	kernel_end = round_up(__pa_symbol(&_end), PAGE_SIZE);
	reserve_bootmem_generic(HIGH_MEMORY, kernel_end - HIGH_MEMORY);

#ifdef CONFIG_X86_LOCAL_APIC
	/*
	 * Find and reserve possible boot-time SMP configuration:
	 */
	find_smp_config();
#endif

#ifdef CONFIG_SMP
	/* AP processor realmode stacks in low memory*/
	smp_alloc_memory();
#endif

	paging_init();
#ifdef CONFIG_X86_LOCAL_APIC
	/*
	 * get boot-time SMP configuration:
	 */
	if (smp_found_config)
		get_smp_config();
	init_apic_mappings();	
#endif

	/*
	 * Request address space for all standard RAM and ROM resources
	 * and also for regions reported as reserved by the e820.
	 */
	probe_roms();
	e820_reserve_resources(); 
	request_resource(&iomem_resource, &vram_resource);

	/* request I/O space for devices used on all i[345]86 PCs */
	for (i = 0; i < STANDARD_IO_RESOURCES; i++)
		request_resource(&ioport_resource, standard_io_resources+i);

	/* We put PCI memory up to make sure VALID_PAGE with DISCONTIGMEM
	   never returns true for it */ 

	/* Tell the PCI layer not to allocate too close to the RAM area.. */
	pci_mem_start = IOMAP_START;

#ifdef CONFIG_GART_IOMMU
	iommu_hole_init();
#endif

#ifdef CONFIG_VT
#if defined(CONFIG_VGA_CONSOLE)
	conswitchp = &vga_con;
#elif defined(CONFIG_DUMMY_CONSOLE)
	conswitchp = &dummy_con;
#endif
#endif

	num_mappedpages = end_pfn;
}

#ifndef CONFIG_X86_TSC
static int tsc_disable __initdata = 0;

static int __init tsc_setup(char *str)
{
	tsc_disable = 1;
	return 1;
}

__setup("notsc", tsc_setup);
#endif

static int __init get_model_name(struct cpuinfo_x86 *c)
{
	unsigned int *v;

	if (cpuid_eax(0x80000000) < 0x80000004)
		return 0;

	v = (unsigned int *) c->x86_model_id;
	cpuid(0x80000002, &v[0], &v[1], &v[2], &v[3]);
	cpuid(0x80000003, &v[4], &v[5], &v[6], &v[7]);
	cpuid(0x80000004, &v[8], &v[9], &v[10], &v[11]);
	c->x86_model_id[48] = 0;
	return 1;
}


static void __init display_cacheinfo(struct cpuinfo_x86 *c)
{
	unsigned int n, dummy, ecx, edx, eax, ebx, eax_2, ebx_2, ecx_2;

	n = cpuid_eax(0x80000000);


	if (n >= 0x80000005) {
		if (n >= 0x80000006) 
			cpuid(0x80000006, &eax_2, &ebx_2, &ecx_2, &dummy); 
	
		cpuid(0x80000005, &eax, &ebx, &ecx, &edx);
		printk(KERN_INFO "CPU: L1 I Cache: %dK (%d bytes/line/%d way), D cache %dK (%d bytes/line/%d way)\n",
		       edx>>24, edx&0xFF, (edx>>16)&0xff, 
		       ecx>>24, ecx&0xFF, (ecx>>16)&0xff);
		c->x86_cache_size=(ecx>>24)+(edx>>24);	
		if (n >= 0x80000006) {
			printk(KERN_INFO "CPU: L2 Cache: %dK (%d bytes/line/%d way)\n",
			       ecx_2>>16, ecx_2&0xFF, (ecx_2>>12)&0xf);
			c->x86_cache_size = ecx_2 >> 16;
		}		
		printk(KERN_INFO "CPU: DTLB L1 %d 4K %d 2MB L2: %d 4K %d 2MB\n",
		       (ebx>>16)&0xff, (eax>>16)&0xff, 
		       (ebx_2>>16)&0xfff, (eax_2>>16)&0xfff);
		printk(KERN_INFO "CPU: ITLB L1 %d 4K %d 2MB L2: %d 4K %d 2MB\n",
		       ebx&0xff, (eax)&0xff, 
		       (ebx_2)&0xfff, (eax_2)&0xfff);		
		c->x86_tlbsize = ((ebx>>16)&0xff) + ((ebx_2>>16)&0xfff) + 
			(ebx&0xff) + ((ebx_2)&0xfff);
	}
}

static int __init init_amd(struct cpuinfo_x86 *c)
{
	int r;

	/* Bit 31 in normal CPUID used for nonstandard 3DNow ID;
	   3DNow is IDd by bit 31 in extended CPUID (1*32+31) anyway */
	clear_bit(0*32+31, &c->x86_capability);
	
	r = get_model_name(c);
	if (!r) { 
		switch (c->x86) { 
		case 15:
			/* Should distingush Models here, but this is only
			   a fallback anyways. */
			strcpy(c->x86_model_id, "Hammer");
			break; 
		} 
	} 
	display_cacheinfo(c);
	return r;
}


void __init get_cpu_vendor(struct cpuinfo_x86 *c)
{
	char *v = c->x86_vendor_id;

	if (!strcmp(v, "AuthenticAMD"))
		c->x86_vendor = X86_VENDOR_AMD;
	else
		c->x86_vendor = X86_VENDOR_UNKNOWN;
}

struct cpu_model_info {
	int vendor;
	int family;
	char *model_names[16];
};

int __init x86_fxsr_setup(char * s)
{
	disable_x86_fxsr = 1;
	return 1;
}
__setup("nofxsr", x86_fxsr_setup);



/*
 * This does the hard work of actually picking apart the CPU stuff...
 */
void __init identify_cpu(struct cpuinfo_x86 *c)
{
	int junk, i;
	u32 xlvl, tfms;

	c->loops_per_jiffy = loops_per_jiffy;
	c->x86_cache_size = -1;
	c->x86_vendor = X86_VENDOR_UNKNOWN;
	c->x86_model = c->x86_mask = 0;	/* So far unknown... */
	c->x86_vendor_id[0] = '\0'; /* Unset */
	c->x86_model_id[0] = '\0';  /* Unset */
	memset(&c->x86_capability, 0, sizeof c->x86_capability);

	/* Get vendor name */
	cpuid(0x00000000, &c->cpuid_level,
	      (int *)&c->x86_vendor_id[0],
	      (int *)&c->x86_vendor_id[8],
	      (int *)&c->x86_vendor_id[4]);
		
	get_cpu_vendor(c);
	/* Initialize the standard set of capabilities */
	/* Note that the vendor-specific code below might override */

	/* Intel-defined flags: level 0x00000001 */
	if ( c->cpuid_level >= 0x00000001 ) {	
		__u32 misc;
		cpuid(0x00000001, &tfms, &misc, &junk,
		      &c->x86_capability[0]);
		c->x86 = (tfms >> 8) & 15;
		if (c->x86 == 0xf)
			c->x86 += (tfms >> 20) & 0xff;
		c->x86_model = (tfms >> 4) & 15;
		if (c->x86_model == 0xf)
			c->x86_model += ((tfms >> 16) & 0xF) << 4; 
		c->x86_mask = tfms & 15;
		if (c->x86_capability[0] & (1<<19)) 
			c->x86_clflush_size = ((misc >> 8) & 0xff) * 8;
	} else {
		/* Have CPUID level 0 only - unheard of */
		c->x86 = 4;
	}

	/* AMD-defined flags: level 0x80000001 */
	xlvl = cpuid_eax(0x80000000);
	if ( (xlvl & 0xffff0000) == 0x80000000 ) {
		if ( xlvl >= 0x80000001 )
			c->x86_capability[1] = cpuid_edx(0x80000001);
		if ( xlvl >= 0x80000004 )
			get_model_name(c); /* Default name */
	}

	/* Transmeta-defined flags: level 0x80860001 */
	xlvl = cpuid_eax(0x80860000);
	if ( (xlvl & 0xffff0000) == 0x80860000 ) {
		if (  xlvl >= 0x80860001 )
			c->x86_capability[2] = cpuid_edx(0x80860001);
	}


	/*
	 * Vendor-specific initialization.  In this section we
	 * canonicalize the feature flags, meaning if there are
	 * features a certain CPU supports which CPUID doesn't
	 * tell us, CPUID claiming incorrect flags, or other bugs,
	 * we handle them here.
	 *
	 * At the end of this section, c->x86_capability better
	 * indicate the features this CPU genuinely supports!
	 */
	switch ( c->x86_vendor ) {

		case X86_VENDOR_AMD:
			init_amd(c);
			break;

		case X86_VENDOR_UNKNOWN:
		default:
			/* Not much we can do here... */
			break;
	}

	/*
	 * The vendor-specific functions might have changed features.  Now
	 * we do "generic changes."
	 */

	/* TSC disabled? */
#ifndef CONFIG_X86_TSC
	if ( tsc_disable )
		clear_bit(X86_FEATURE_TSC, &c->x86_capability);
#endif

	/* FXSR disabled? */
	if (disable_x86_fxsr) {
		clear_bit(X86_FEATURE_FXSR, &c->x86_capability);
		clear_bit(X86_FEATURE_XMM, &c->x86_capability);
	}

	/*
	 * On SMP, boot_cpu_data holds the common feature set between
	 * all CPUs; so make sure that we indicate which features are
	 * common between the CPUs.  The first time this routine gets
	 * executed, c == &boot_cpu_data.
	 */
	if ( c != &boot_cpu_data ) {
		/* AND the already accumulated flags with these */
		for ( i = 0 ; i < NCAPINTS ; i++ )
			boot_cpu_data.x86_capability[i] &= c->x86_capability[i];
	}

#ifdef CONFIG_MCE
	mcheck_init(c);
#endif
}
 
void __init print_cpu_info(struct cpuinfo_x86 *c)
{
	if (c->x86_model_id[0])
		printk("%s", c->x86_model_id);

	if (c->x86_mask || c->cpuid_level >= 0) 
		printk(" stepping %02x\n", c->x86_mask);
	else
		printk("\n");
}

/*
 *	Get CPU information for use by the procfs.
 */

static int show_cpuinfo(struct seq_file *m, void *v)
{
	struct cpuinfo_x86 *c = v;

	/* 
	 * These flag bits must match the definitions in <asm/cpufeature.h>.
	 * NULL means this bit is undefined or reserved; either way it doesn't
	 * have meaning as far as Linux is concerned.  Note that it's important
	 * to realize there is a difference between this table and CPUID -- if
	 * applications want to get the raw CPUID data, they should access
	 * /dev/cpu/<cpu_nr>/cpuid instead.
	 */
	static char *x86_cap_flags[] = {
		/* Intel-defined */
	        "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce",
	        "cx8", "apic", NULL, "sep", "mtrr", "pge", "mca", "cmov",
	        "pat", "pse36", "pn", "clflush", NULL, "dts", "acpi", "mmx",
	        "fxsr", "sse", "sse2", "ss", NULL, "tm", "ia64", NULL,

		/* AMD-defined */
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, "syscall", NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, "nx", NULL, "mmxext", NULL,
		NULL, NULL, NULL, NULL, NULL, "lm", "3dnowext", "3dnow",

		/* Transmeta-defined */
		"recovery", "longrun", NULL, "lrti", NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,

		/* Other (Linux-defined) */
		"cxmmx", "k6_mtrr", "cyrix_arr", "centaur_mcr", NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	};

#ifdef CONFIG_SMP
	if (!(cpu_online_map & (1<<(c-cpu_data))))
		return 0;
#endif

	seq_printf(m,"processor\t: %u\n"
		     "vendor_id\t: %s\n"
		     "cpu family\t: %d\n"
		     "model\t\t: %d\n"
		     "model name\t: %s\n",
		     (unsigned)(c-cpu_data),
		     c->x86_vendor_id[0] ? c->x86_vendor_id : "unknown",
		     c->x86,
		     (int)c->x86_model,
		     c->x86_model_id[0] ? c->x86_model_id : "unknown");
	
	if (c->x86_mask || c->cpuid_level >= 0)
		seq_printf(m, "stepping\t: %d\n", c->x86_mask);
	else
		seq_printf(m, "stepping\t: unknown\n");
	
	if ( test_bit(X86_FEATURE_TSC, &c->x86_capability) ) {
		seq_printf(m, "cpu MHz\t\t: %u.%03u\n",
			     cpu_khz / 1000, (cpu_khz % 1000));
	}

	seq_printf(m, "cache size\t: %d KB\n", c->x86_cache_size);
	
	seq_printf(m,
	        "fpu\t\t: yes\n"
	        "fpu_exception\t: yes\n"
	        "cpuid level\t: %d\n"
	        "wp\t\t: yes\n"
	        "flags\t\t:",
		   c->cpuid_level);

	{ 
		int i; 
		for ( i = 0 ; i < 32*NCAPINTS ; i++ )
			if ( test_bit(i, &c->x86_capability) &&
			     x86_cap_flags[i] != NULL )
				seq_printf(m, " %s", x86_cap_flags[i]);
	}
		
	seq_printf(m, "\nbogomips\t: %lu.%02lu\n",
		   c->loops_per_jiffy/(500000/HZ),
		   (c->loops_per_jiffy/(5000/HZ)) % 100);

	if (c->x86_tlbsize > 0) 
		seq_printf(m, "TLB size\t: %d 4K pages\n", c->x86_tlbsize);
	seq_printf(m, "clflush size\t: %d\n", c->x86_clflush_size);

	seq_printf(m, "\n"); 
	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < NR_CPUS ? cpu_data + *pos : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

struct seq_operations cpuinfo_op = {
	start:	c_start,
	next:	c_next,
	stop:	c_stop,
	show:	show_cpuinfo,
};
