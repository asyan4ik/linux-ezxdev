/*
 * Common Flash Interface support:
 *   Intel Extended Vendor Command Set (ID 0x0001)
 *
 * (C) 2000 Red Hat. GPL'd
 * Copyright (C) 2005 Motorola Inc.
 *
 * $Id: cfi_cmdset_0001.c,v 1.155 2004/08/12 06:40:22 eric Exp $
 *
 * 
 * 10/10/2000	Nicolas Pitre <nico@cam.org>
 * 	- completely revamped method functions so they are aware and
 * 	  independent of the flash geometry (buswidth, interleave, etc.)
 * 	- scalability vs code size is completely set at compile-time
 * 	  (see include/linux/mtd/cfi.h for selection)
 *	- optimized write buffer method
 * 02/05/2002	Christopher Hoover <ch@hpl.hp.com>/<ch@murgatroid.com>
 *	- reworked lock/unlock/erase support for var size flash
 * 01/15/2005  Susan Gu <w15879@motorola.com>
 *      - Modified for EzX and pm support 
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/byteorder.h>

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mtd/xip.h>
#include <linux/mtd/map.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/compatmac.h>
#include <linux/mtd/cfi.h>

#ifdef CONFIG_ARCH_EZX

#include <linux/pm.h>

static struct fl_rw_region fl_rw_array[] = {
	{		
		/* This area contains "CADDO2"+"ITUNES"+"CADDO1"+"FOTA_REV" */
		rw_start: 0x00000000,
		rw_end:  0x00020000,
	},{		
		/* This partition contains "userfs_db"+"userfs_general" */
		rw_start: 0x00060000,
		rw_end:  0x018C0000,
	},{		
		/* This partition contains "tcmd"+"logo"+"fota"+"128KB reserve" */	
		rw_start: 0x018C0000,
		rw_end:  0x04000000,
	}
};

#define TIME_OUT   1000000L

/* This is for notification of flash status before we are asked entering into sleep mode -- Susan */
#ifdef CONFIG_PM
struct pm_flash_s pm_flash;
#endif

struct mtd_info *ezx_mymtd;
extern struct map_info bulverde_map;  //Susan
extern unsigned long bulverde_map_cacheable;  //Susan
int cfi_intelext_read_roflash(void *priv_map, unsigned long from, size_t len, size_t *retlen, u_char *buf);

#endif  //CONFIG_ARCH_EZX

/* #define CMDSET0001_DISABLE_ERASE_SUSPEND_ON_WRITE */
/* #define CMDSET0001_DISABLE_WRITE_SUSPEND */

// debugging, turns off buffer write mode if set to 1
#define FORCE_WORD_WRITE 0

#define MANUFACTURER_INTEL	0x0089
#define I82802AB	0x00ad
#define I82802AC	0x00ac

static int cfi_intelext_read (struct mtd_info *, loff_t, size_t, size_t *, u_char *);
static int cfi_intelext_write_words(struct mtd_info *, loff_t, size_t, size_t *, const u_char *);
static int cfi_intelext_write_buffers(struct mtd_info *, loff_t, size_t, size_t *, const u_char *);
static int cfi_intelext_erase_varsize(struct mtd_info *, struct erase_info *);
static void cfi_intelext_sync (struct mtd_info *);
static int cfi_intelext_lock(struct mtd_info *mtd, loff_t ofs, size_t len);
static int cfi_intelext_unlock(struct mtd_info *mtd, loff_t ofs, size_t len);
static int cfi_intelext_read_fact_prot_reg (struct mtd_info *, loff_t, size_t, size_t *, u_char *);
static int cfi_intelext_read_user_prot_reg (struct mtd_info *, loff_t, size_t, size_t *, u_char *);
static int cfi_intelext_write_user_prot_reg (struct mtd_info *, loff_t, size_t, size_t *, u_char *);
static int cfi_intelext_lock_user_prot_reg (struct mtd_info *, loff_t, size_t);
static int cfi_intelext_get_fact_prot_info (struct mtd_info *,
					    struct otp_info *, size_t);
static int cfi_intelext_get_user_prot_info (struct mtd_info *,
					    struct otp_info *, size_t);
static int cfi_intelext_suspend (struct mtd_info *);
static void cfi_intelext_resume (struct mtd_info *);

static void cfi_intelext_destroy(struct mtd_info *);

struct mtd_info *cfi_cmdset_0001(struct map_info *, int);

static struct mtd_info *cfi_intelext_setup (struct mtd_info *);
static int cfi_intelext_partition_fixup(struct mtd_info *, struct cfi_private **);

static int cfi_intelext_point (struct mtd_info *mtd, loff_t from, size_t len,
		     size_t *retlen, u_char **mtdbuf);
static void cfi_intelext_unpoint (struct mtd_info *mtd, u_char *addr, loff_t from,
			size_t len);

static int get_chip(struct map_info *map, struct flchip *chip, unsigned long adr, int mode);
static void put_chip(struct map_info *map, struct flchip *chip, unsigned long adr);
#include "fwh_lock.h"



/*
 *  *********** SETUP AND PROBE BITS  ***********
 */

static struct mtd_chip_driver cfi_intelext_chipdrv = {
	.probe		= NULL, /* Not usable directly */
	.destroy	= cfi_intelext_destroy,
	.name		= "cfi_cmdset_0001",
	.module		= THIS_MODULE
};

/* #define DEBUG_LOCK_BITS */
/* #define DEBUG_CFI_FEATURES */

#ifdef DEBUG_CFI_FEATURES
static void cfi_tell_features(struct cfi_pri_intelext *extp)
{
	int i;
	printk("  Feature/Command Support:      %4.4X\n", extp->FeatureSupport);
	printk("     - Chip Erase:              %s\n", extp->FeatureSupport&1?"supported":"unsupported");
	printk("     - Suspend Erase:           %s\n", extp->FeatureSupport&2?"supported":"unsupported");
	printk("     - Suspend Program:         %s\n", extp->FeatureSupport&4?"supported":"unsupported");
	printk("     - Legacy Lock/Unlock:      %s\n", extp->FeatureSupport&8?"supported":"unsupported");
	printk("     - Queued Erase:            %s\n", extp->FeatureSupport&16?"supported":"unsupported");
	printk("     - Instant block lock:      %s\n", extp->FeatureSupport&32?"supported":"unsupported");
	printk("     - Protection Bits:         %s\n", extp->FeatureSupport&64?"supported":"unsupported");
	printk("     - Page-mode read:          %s\n", extp->FeatureSupport&128?"supported":"unsupported");
	printk("     - Synchronous read:        %s\n", extp->FeatureSupport&256?"supported":"unsupported");
	printk("     - Simultaneous operations: %s\n", extp->FeatureSupport&512?"supported":"unsupported");
	for (i=10; i<32; i++) {
		if (extp->FeatureSupport & (1<<i)) 
			printk("     - Unknown Bit %X:      supported\n", i);
	}
	
	printk("  Supported functions after Suspend: %2.2X\n", extp->SuspendCmdSupport);
	printk("     - Program after Erase Suspend: %s\n", extp->SuspendCmdSupport&1?"supported":"unsupported");
	for (i=1; i<8; i++) {
		if (extp->SuspendCmdSupport & (1<<i))
			printk("     - Unknown Bit %X:               supported\n", i);
	}
	
	printk("  Block Status Register Mask: %4.4X\n", extp->BlkStatusRegMask);
	printk("     - Lock Bit Active:      %s\n", extp->BlkStatusRegMask&1?"yes":"no");
	printk("     - Valid Bit Active:     %s\n", extp->BlkStatusRegMask&2?"yes":"no");
	for (i=2; i<16; i++) {
		if (extp->BlkStatusRegMask & (1<<i))
			printk("     - Unknown Bit %X Active: yes\n",i);
	}
	
	printk("  Vcc Logic Supply Optimum Program/Erase Voltage: %d.%d V\n", 
	       extp->VccOptimal >> 4, extp->VccOptimal & 0xf);
	if (extp->VppOptimal)
		printk("  Vpp Programming Supply Optimum Program/Erase Voltage: %d.%d V\n", 
		       extp->VppOptimal >> 4, extp->VppOptimal & 0xf);
}
#endif

#ifdef CMDSET0001_DISABLE_ERASE_SUSPEND_ON_WRITE
/* Some Intel Strata Flash prior to FPO revision C has bugs in this area */ 
static void fixup_intel_strataflash(struct mtd_info *mtd, void* param)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_amdstd *extp = cfi->cmdset_priv;

	printk(KERN_WARNING "cfi_cmdset_0001: Suspend "
	                    "erase on write disabled.\n");
	extp->SuspendCmdSupport &= ~1;
}
#endif

#ifdef CMDSET0001_DISABLE_WRITE_SUSPEND
static void fixup_no_write_suspend(struct mtd_info *mtd, void* param)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_intelext *cfip = cfi->cmdset_priv;

	if (cfip && (cfip->FeatureSupport&4)) {
		cfip->FeatureSupport &= ~4;
		printk(KERN_WARNING "cfi_cmdset_0001: write suspend disabled\n");
	}
}
#endif

static void fixup_st_m28w320ct(struct mtd_info *mtd, void* param)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	
	cfi->cfiq->BufWriteTimeoutTyp = 0;	/* Not supported */
	cfi->cfiq->BufWriteTimeoutMax = 0;	/* Not supported */
}

static void fixup_st_m28w320cb(struct mtd_info *mtd, void* param)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	
	/* Note this is done after the region info is endian swapped */
	cfi->cfiq->EraseRegionInfo[1] =
		(cfi->cfiq->EraseRegionInfo[1] & 0xffff0000) | 0x3e;
};

static void fixup_use_point(struct mtd_info *mtd, void *param)
{
	struct map_info *map = mtd->priv;
	if (!mtd->point && map_is_linear(map)) {
		mtd->point   = cfi_intelext_point;
		mtd->unpoint = cfi_intelext_unpoint;
	}
}

static void fixup_use_write_buffers(struct mtd_info *mtd, void *param)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	if (cfi->cfiq->BufWriteTimeoutTyp) {
		printk(KERN_INFO "Using buffer write method\n" );
		mtd->write = cfi_intelext_write_buffers;
	}
}

static struct cfi_fixup cfi_fixup_table[] = {
#ifdef CMDSET0001_DISABLE_ERASE_SUSPEND_ON_WRITE
	{ CFI_MFR_ANY, CFI_ID_ANY, fixup_intel_strataflash, NULL }, 
#endif
#ifdef CMDSET0001_DISABLE_WRITE_SUSPEND
	{ CFI_MFR_ANY, CFI_ID_ANY, fixup_no_write_suspend, NULL },
#endif
#if !FORCE_WORD_WRITE
	{ CFI_MFR_ANY, CFI_ID_ANY, fixup_use_write_buffers, NULL },
#endif
	{ CFI_MFR_ST, 0x00ba, /* M28W320CT */ fixup_st_m28w320ct, NULL },
	{ CFI_MFR_ST, 0x00bb, /* M28W320CB */ fixup_st_m28w320cb, NULL },
	{ 0, 0, NULL, NULL }
};

static struct cfi_fixup jedec_fixup_table[] = {
	{ MANUFACTURER_INTEL, I82802AB, fixup_use_fwh_lock, NULL, },
	{ MANUFACTURER_INTEL, I82802AC, fixup_use_fwh_lock, NULL, },
	{ 0, 0, NULL, NULL }
};
static struct cfi_fixup fixup_table[] = {
	/* The CFI vendor ids and the JEDEC vendor IDs appear
	 * to be common.  It is like the devices id's are as
	 * well.  This table is to pick all cases where
	 * we know that is the case.
	 */
	{ CFI_MFR_ANY, CFI_ID_ANY, fixup_use_point, NULL },
	{ 0, 0, NULL, NULL }
};

static inline struct cfi_pri_intelext *
read_pri_intelext(struct map_info *map, __u16 adr)
{
	struct cfi_pri_intelext *extp;
	unsigned int extp_size = sizeof(*extp);

 again:
	extp = (struct cfi_pri_intelext *)cfi_read_pri(map, adr, extp_size, "Intel/Sharp");
	if (!extp)
		return NULL;

	/* Do some byteswapping if necessary */
	extp->FeatureSupport = le32_to_cpu(extp->FeatureSupport);
	extp->BlkStatusRegMask = le16_to_cpu(extp->BlkStatusRegMask);
	extp->ProtRegAddr = le16_to_cpu(extp->ProtRegAddr);

	if (extp->MajorVersion == '1' && extp->MinorVersion == '3') {
		unsigned int extra_size = 0;
		int nb_parts, i;

		/* Protection Register info */
		extra_size += (extp->NumProtectionFields - 1) *
			      sizeof(struct cfi_intelext_otpinfo);
			      

		if (extp_size < sizeof(*extp) + extra_size) {
			need_more:
			extp_size = sizeof(*extp) + extra_size;
			kfree(extp);
			if (extp_size > 4096) {
				printk(KERN_ERR
					"%s: cfi_pri_intelext is too fat\n",
					__FUNCTION__);
				return NULL;
			}
			goto again;
		}
	}
		
	return extp;
}

/* This routine is made available to other mtd code via
 * inter_module_register.  It must only be accessed through
 * inter_module_get which will bump the use count of this module.  The
 * addresses passed back in cfi are valid as long as the use count of
 * this module is non-zero, i.e. between inter_module_get and
 * inter_module_put.  Keith Owens <kaos@ocs.com.au> 29 Oct 2000.
 */
struct mtd_info *cfi_cmdset_0001(struct map_info *map, int primary)
{
	struct cfi_private *cfi = map->fldrv_priv;
	struct mtd_info *mtd;
	int i;

	mtd = kmalloc(sizeof(*mtd), GFP_KERNEL);
	if (!mtd) {
		printk(KERN_ERR "Failed to allocate memory for MTD device\n");
		return NULL;
	}
	memset(mtd, 0, sizeof(*mtd));
	mtd->priv = map;
	mtd->type = MTD_NORFLASH;

	/* Fill in the default mtd operations */
	mtd->erase   = cfi_intelext_erase_varsize;
	mtd->read    = cfi_intelext_read;
	mtd->write   = cfi_intelext_write_words;
	mtd->sync    = cfi_intelext_sync;
	mtd->lock    = cfi_intelext_lock;
	mtd->unlock  = cfi_intelext_unlock;
	mtd->suspend = cfi_intelext_suspend;
	mtd->resume  = cfi_intelext_resume;
	mtd->flags   = MTD_CAP_NORFLASH;
	mtd->name    = map->name;
	
	if (cfi->cfi_mode == CFI_MODE_CFI) {
		/* 
		 * It's a real CFI chip, not one for which the probe
		 * routine faked a CFI structure. So we read the feature
		 * table from it.
		 */
		__u16 adr = primary?cfi->cfiq->P_ADR:cfi->cfiq->A_ADR;
		struct cfi_pri_intelext *extp;

		extp = read_pri_intelext(map, adr);
		if (!extp) {
			kfree(mtd);
			return NULL;
		}

		/* Install our own private info structure */
		cfi->cmdset_priv = extp;	

		cfi_fixup(mtd, cfi_fixup_table);

#ifdef DEBUG_CFI_FEATURES
		/* Tell the user about it in lots of lovely detail */
		cfi_tell_features(extp);
#endif	

		if(extp->SuspendCmdSupport & 1) {
			printk(KERN_NOTICE "cfi_cmdset_0001: Erase suspend on write enabled\n");
		}
	}
	else if (cfi->cfi_mode == CFI_MODE_JEDEC) {
		/* Apply jedec specific fixups */
		cfi_fixup(mtd, jedec_fixup_table);
	}
	/* Apply generic fixups */
	cfi_fixup(mtd, fixup_table);

	for (i=0; i< cfi->numchips; i++) {
		cfi->chips[i].word_write_time = 1<<cfi->cfiq->WordWriteTimeoutTyp;
		cfi->chips[i].buffer_write_time = 1<<cfi->cfiq->BufWriteTimeoutTyp;
		cfi->chips[i].erase_time = 1<<cfi->cfiq->BlockEraseTimeoutTyp;
		cfi->chips[i].ref_point_counter = 0;
	}		

	map->fldrv = &cfi_intelext_chipdrv;
	
	return cfi_intelext_setup(mtd);
}

static struct mtd_info *cfi_intelext_setup(struct mtd_info *mtd)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long offset = 0;
	int i,j;
	unsigned long devsize = (1<<cfi->cfiq->DevSize) * cfi->interleave;

	//printk(KERN_DEBUG "number of CFI chips: %d\n", cfi->numchips);

	mtd->size = devsize * cfi->numchips;

	mtd->numeraseregions = cfi->cfiq->NumEraseRegions * cfi->numchips;
	mtd->eraseregions = kmalloc(sizeof(struct mtd_erase_region_info) 
			* mtd->numeraseregions, GFP_KERNEL);
	if (!mtd->eraseregions) { 
		printk(KERN_ERR "Failed to allocate memory for MTD erase region info\n");
		goto setup_err;
	}
	
	//printk(KERN_NOTICE "cfi_intelext_setup:cfi->numchips(%d),mtd->numeraseregions(%d)\n",cfi->numchips,mtd->numeraseregions);
	
	/* Changed by susan for TOP+BOTTOM geometry layout -- Barbados */	
	for (j=0; j<cfi->numchips; j++)
	{
		unsigned short same_as_first = 0;
		
		if (j == 0)
			same_as_first = 1;
		else
		{
			unsigned short num_erblocks_0 = 0;
			unsigned long chip_base = devsize * j;
			
			cfi_send_gen_cmd(0x98, 0x55, chip_base, map, cfi, cfi->device_type, NULL);

			num_erblocks_0 = cfi_read_query(map, chip_base + (0x2D)*(cfi->interleave*cfi->device_type));
			//printk(KERN_NOTICE "cfi_intelext_setup: chip(%d),num_erblocks_0(%d)\n",j,num_erblocks_0);
			
			if ( num_erblocks_0 == ((cfi->cfiq->EraseRegionInfo[0] & 0xFFFF) + 1) )
				same_as_first = 1;
			else
				same_as_first = 0;
					
			cfi_send_gen_cmd(0xFF, 0, chip_base, map, cfi, cfi->device_type, NULL);
		}
		
		offset = 0;	
		for (i=0; i<cfi->cfiq->NumEraseRegions; i++)
		{			
			unsigned long ernum, ersize;
			unsigned char k;
			if (same_as_first)
			{
				ersize = ((cfi->cfiq->EraseRegionInfo[i] >> 8) & ~0xff) * cfi->interleave;
				ernum = (cfi->cfiq->EraseRegionInfo[i] & 0xffff) + 1;
			}
			else
			{
				k = cfi->cfiq->NumEraseRegions - i - 1; //assume onle two different erase regions exist in one Intel flash chip//
				ersize = ((cfi->cfiq->EraseRegionInfo[k] >> 8) & ~0xff) * cfi->interleave;
				ernum = (cfi->cfiq->EraseRegionInfo[k] & 0xffff) + 1;
				
			}
			
			if (mtd->erasesize < ersize) {
				mtd->erasesize = ersize;
			}
				
			mtd->eraseregions[(j*cfi->cfiq->NumEraseRegions)+i].offset = (j*devsize)+offset;
			mtd->eraseregions[(j*cfi->cfiq->NumEraseRegions)+i].erasesize = ersize;
			mtd->eraseregions[(j*cfi->cfiq->NumEraseRegions)+i].numblocks = ernum;

			//printk(KERN_NOTICE "cfi_intelext_setup:mtd->eraseregions[%d]--offset(%d),erasesize(%d),numblocks(%d)\n",((j*cfi->cfiq->NumEraseRegions)+i),((j*devsize)+offset),ersize,ernum);

			offset += (ersize * ernum);
		}
	}

	if (offset != devsize) {
		/* Argh */
		printk(KERN_WARNING "Sum of regions (%lx) != total size of set of interleaved chips (%lx)\n", offset, devsize);
		goto setup_err;
	}

	for (i=0; i<mtd->numeraseregions;i++){
		printk(KERN_DEBUG "%d: offset=0x%x,size=0x%x,blocks=%d\n",
		       i,mtd->eraseregions[i].offset,
		       mtd->eraseregions[i].erasesize,
		       mtd->eraseregions[i].numblocks);
	}

#if 1
	mtd->read_fact_prot_reg = cfi_intelext_read_fact_prot_reg;
	mtd->read_user_prot_reg = cfi_intelext_read_user_prot_reg;
	mtd->write_user_prot_reg = cfi_intelext_write_user_prot_reg;
	mtd->lock_user_prot_reg = cfi_intelext_lock_user_prot_reg;
	mtd->get_fact_prot_info = cfi_intelext_get_fact_prot_info;
	mtd->get_user_prot_info = cfi_intelext_get_user_prot_info;
#endif

	/* This function has the potential to distort the reality
	   a bit and therefore should be called last. */
	if (cfi_intelext_partition_fixup(mtd, &cfi) != 0)
		goto setup_err;

	__module_get(THIS_MODULE);
	return mtd;

 setup_err:
	if(mtd) {
		if(mtd->eraseregions)
			kfree(mtd->eraseregions);
		kfree(mtd);
	}
	kfree(cfi->cmdset_priv);
	return NULL;
}

static int cfi_intelext_partition_fixup(struct mtd_info *mtd,
					struct cfi_private **pcfi)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = *pcfi;
	struct cfi_pri_intelext *extp = cfi->cmdset_priv;

	/*
	 * Probing of multi-partition flash ships.
	 *
	 * To support multiple partitions when available, we simply arrange
	 * for each of them to have their own flchip structure even if they
	 * are on the same physical chip.  This means completely recreating
	 * a new cfi_private structure right here which is a blatent code
	 * layering violation, but this is still the least intrusive
	 * arrangement at this point. This can be rearranged in the future
	 * if someone feels motivated enough.  --nico
	 */
	if (extp && extp->MajorVersion == '1' && extp->MinorVersion == '3'
	    && extp->FeatureSupport & (1 << 9)) {
		struct cfi_private *newcfi;
		struct flchip *chip;
		struct flchip_shared *shared;
		int offs, numregions, numparts, partshift, numvirtchips, i, j;

		/* Protection Register info */
		offs = (extp->NumProtectionFields - 1) *
		       sizeof(struct cfi_intelext_otpinfo);

		/* Burst Read info */
		offs += 6;

		/* Number of partition regions */
		numregions = extp->extra[offs];
		offs += 1;

		/* Number of hardware partitions */
		numparts = 0;
		for (i = 0; i < numregions; i++) {
			struct cfi_intelext_regioninfo *rinfo;
			rinfo = (struct cfi_intelext_regioninfo *)&extp->extra[offs];
			numparts += rinfo->NumIdentPartitions;
			offs += sizeof(*rinfo)
				+ (rinfo->NumBlockTypes - 1) *
				  sizeof(struct cfi_intelext_blockinfo);
		}

		/*
		 * All functions below currently rely on all chips having
		 * the same geometry so we'll just assume that all hardware
		 * partitions are of the same size too.
		 */
		partshift = cfi->chipshift - __ffs(numparts);

		if ((1 << partshift) < mtd->erasesize) {
			printk( KERN_ERR
				"%s: bad number of hw partitions (%d)\n",
				__FUNCTION__, numparts);
			return -EINVAL;
		}

		numvirtchips = cfi->numchips * numparts;
		newcfi = kmalloc(sizeof(struct cfi_private) + numvirtchips * sizeof(struct flchip), GFP_KERNEL);
		if (!newcfi)
			return -ENOMEM;
		shared = kmalloc(sizeof(struct flchip_shared) * cfi->numchips, GFP_KERNEL);
		if (!shared) {
			kfree(newcfi);
			return -ENOMEM;
		}
		memcpy(newcfi, cfi, sizeof(struct cfi_private));
		newcfi->numchips = numvirtchips;
		newcfi->chipshift = partshift;

		chip = &newcfi->chips[0];
		for (i = 0; i < cfi->numchips; i++) {
			shared[i].writing = shared[i].erasing = NULL;
			spin_lock_init(&shared[i].lock);
			for (j = 0; j < numparts; j++) {
				*chip = cfi->chips[i];
				chip->start += j << partshift;
				chip->priv = &shared[i];
				/* those should be reset too since
				   they create memory references. */
				init_waitqueue_head(&chip->wq);
				spin_lock_init(&chip->_spinlock);
				chip->mutex = &chip->_spinlock;
				chip++;
			}
		}

		printk(KERN_DEBUG "%s: %d set(s) of %d interleaved chips "
				  "--> %d partitions of %d KiB\n",
				  map->name, cfi->numchips, cfi->interleave,
				  newcfi->numchips, 1<<(newcfi->chipshift-10));

		map->fldrv_priv = newcfi;
		*pcfi = newcfi;
		kfree(cfi);
	}

	return 0;
}

/*
 *  *********** CHIP ACCESS FUNCTIONS ***********
 */

static int get_chip(struct map_info *map, struct flchip *chip, unsigned long adr, int mode)
{
	DECLARE_WAITQUEUE(wait, current);
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK = CMD(0x80), status_PWS = CMD(0x01);
	unsigned long timeo;
	struct cfi_pri_intelext *cfip = cfi->cmdset_priv;

 resettime:
	timeo = jiffies + HZ;
 retry:
	if (chip->priv && (mode == FL_WRITING || mode == FL_ERASING || mode == FL_OTP_WRITE)) {
		/*
		 * OK. We have possibility for contension on the write/erase
		 * operations which are global to the real chip and not per
		 * partition.  So let's fight it over in the partition which
		 * currently has authority on the operation.
		 *
		 * The rules are as follows:
		 *
		 * - any write operation must own shared->writing.
		 *
		 * - any erase operation must own _both_ shared->writing and
		 *   shared->erasing.
		 *
		 * - contension arbitration is handled in the owner's context.
		 *
		 * The 'shared' struct can be read when its lock is taken.
		 * However any writes to it can only be made when the current
		 * owner's lock is also held.
		 */
		struct flchip_shared *shared = chip->priv;
		struct flchip *contender;
		spin_lock(&shared->lock);
		contender = shared->writing;
		if (contender && contender != chip) {
			/*
			 * The engine to perform desired operation on this
			 * partition is already in use by someone else.
			 * Let's fight over it in the context of the chip
			 * currently using it.  If it is possible to suspend,
			 * that other partition will do just that, otherwise
			 * it'll happily send us to sleep.  In any case, when
			 * get_chip returns success we're clear to go ahead.
			 */
			int ret = spin_trylock(contender->mutex);
			spin_unlock(&shared->lock);
			if (!ret)
				goto retry;
			spin_unlock(chip->mutex);
			ret = get_chip(map, contender, contender->start, mode);
			spin_lock(chip->mutex);
			if (ret) {
				spin_unlock(contender->mutex);
				return ret;
			}
			timeo = jiffies + HZ;
			spin_lock(&shared->lock);
		}

		/* We now own it */
		shared->writing = chip;
		if (mode == FL_ERASING)
			shared->erasing = chip;
		if (contender && contender != chip)
			spin_unlock(contender->mutex);
		spin_unlock(&shared->lock);
	}

	switch (chip->state) {

	case FL_STATUS:
		for (;;) {
			status = map_read(map, adr);
			if (map_word_andequal(map, status, status_OK, status_OK))
				break;

			/* At this point we're fine with write operations
			   in other partitions as they don't conflict. */
			if (chip->priv && map_word_andequal(map, status, status_PWS, status_PWS))
				break;

			if (time_after(jiffies, timeo)) {
				printk(KERN_ERR "Waiting for chip to be ready timed out. Status %lx\n", 
				       status.x[0]);
				return -EIO;
			}
			spin_unlock(chip->mutex);
			cfi_udelay(1);
			spin_lock(chip->mutex);
			/* Someone else might have been playing with it. */
			goto retry;
		}
				
	case FL_READY:
	case FL_CFI_QUERY:
	case FL_JEDEC_QUERY:
		return 0;

	case FL_ERASING:
		if (!cfip ||
		    !(cfip->FeatureSupport & 2) ||
		    !(mode == FL_READY || mode == FL_POINT ||
		     (mode == FL_WRITING && (cfip->SuspendCmdSupport & 1))))
			goto sleep;


		/* Erase suspend */
		map_write(map, CMD(0xB0), adr);

		/* If the flash has finished erasing, then 'erase suspend'
		 * appears to make some (28F320) flash devices switch to
		 * 'read' mode.  Make sure that we switch to 'read status'
		 * mode so we get the right data. --rmk
		 */
		map_write(map, CMD(0x70), adr);
		chip->oldstate = FL_ERASING;
		chip->state = FL_ERASE_SUSPENDING;
		chip->erase_suspended = 1;
		for (;;) {
			status = map_read(map, adr);
			if (map_word_andequal(map, status, status_OK, status_OK))
			        break;

			if (time_after(jiffies, timeo)) {
				/* Urgh. Resume and pretend we weren't here.  */
				map_write(map, CMD(0xd0), adr);
				/* Make sure we're in 'read status' mode if it had finished */
				map_write(map, CMD(0x70), adr);
				chip->state = FL_ERASING;
				chip->oldstate = FL_READY;
				printk(KERN_ERR "Chip not ready after erase "
				       "suspended: status = 0x%lx\n", status.x[0]);
				return -EIO;
			}

			spin_unlock(chip->mutex);
			cfi_udelay(1);
			spin_lock(chip->mutex);
			/* Nobody will touch it while it's in state FL_ERASE_SUSPENDING.
			   So we can just loop here. */
		}
		chip->state = FL_STATUS;
		return 0;

	case FL_XIP_WHILE_ERASING:
		if (mode != FL_READY && mode != FL_POINT &&
		    (mode != FL_WRITING || !cfip || !(cfip->SuspendCmdSupport&1)))
			goto sleep;
		chip->oldstate = chip->state;
		chip->state = FL_READY;
		return 0;

	case FL_POINT:
		/* Only if there's no operation suspended... */
		if (mode == FL_READY && chip->oldstate == FL_READY)
			return 0;

	default:
	sleep:
		set_current_state(TASK_UNINTERRUPTIBLE);
		add_wait_queue(&chip->wq, &wait);
		spin_unlock(chip->mutex);
		schedule();
		remove_wait_queue(&chip->wq, &wait);
		spin_lock(chip->mutex);
		goto resettime;
	}
}

static void put_chip(struct map_info *map, struct flchip *chip, unsigned long adr)
{
	struct cfi_private *cfi = map->fldrv_priv;

	if (chip->priv) {
		struct flchip_shared *shared = chip->priv;
		spin_lock(&shared->lock);
		if (shared->writing == chip && chip->oldstate == FL_READY) {
			/* We own the ability to write, but we're done */
			shared->writing = shared->erasing;
			if (shared->writing && shared->writing != chip) {
				/* give back ownership to who we loaned it from */
				struct flchip *loaner = shared->writing;
				spin_lock(loaner->mutex);
				spin_unlock(&shared->lock);
				spin_unlock(chip->mutex);
				put_chip(map, loaner, loaner->start);
				spin_lock(chip->mutex);
				spin_unlock(loaner->mutex);
				wake_up(&chip->wq);
				return;
			}
			shared->erasing = NULL;
			shared->writing = NULL;
		} else if (shared->erasing == chip && shared->writing != chip) {
			/*
			 * We own the ability to erase without the ability
			 * to write, which means the erase was suspended
			 * and some other partition is currently writing.
			 * Don't let the switch below mess things up since
			 * we don't have ownership to resume anything.
			 */
			spin_unlock(&shared->lock);
			wake_up(&chip->wq);
			return;
		}
		spin_unlock(&shared->lock);
	}

	switch(chip->oldstate) {
	case FL_ERASING:
		chip->state = chip->oldstate;
		/* What if one interleaved chip has finished and the 
		   other hasn't? The old code would leave the finished
		   one in READY mode. That's bad, and caused -EROFS 
		   errors to be returned from do_erase_oneblock because
		   that's the only bit it checked for at the time.
		   As the state machine appears to explicitly allow 
		   sending the 0x70 (Read Status) command to an erasing
		   chip and expecting it to be ignored, that's what we 
		   do. */
		map_write(map, CMD(0xd0), adr);
		map_write(map, CMD(0x70), adr);
		chip->oldstate = FL_READY;
		chip->state = FL_ERASING;
		break;

	case FL_XIP_WHILE_ERASING:
		chip->state = chip->oldstate;
		chip->oldstate = FL_READY;
		break;

	case FL_READY:
	case FL_STATUS:
	case FL_JEDEC_QUERY:
		/* We should really make set_vpp() count, rather than doing this */
		DISABLE_VPP(map);
		break;
	default:
		printk(KERN_ERR "put_chip() called with oldstate %d!!\n", chip->oldstate);
	}
	wake_up(&chip->wq);
}

#ifdef CONFIG_MTD_XIP

/*
 * No interrupt what so ever can be serviced while the flash isn't in array
 * mode.  This is ensured by the xip_disable() and xip_enable() functions
 * enclosing any code path where the flash is known not to be in array mode.
 * And within a XIP disabled code path, only functions marked with __xipram
 * may be called and nothing else (it's a good thing to inspect generated
 * assembly to make sure inline functions were actually inlined and that gcc
 * didn't emit calls to its own support functions). Also configuring MTD CFI
 * support to a single buswidth and a single interleave is also recommended.
 * Note that not only IRQs are disabled but the preemption count is also
 * increased to prevent other locking primitives (namely spin_unlock) from
 * decrementing the preempt count to zero and scheduling the CPU away while
 * not in array mode.
 */

static void xip_disable(struct map_info *map, struct flchip *chip,
			unsigned long adr)
{
	/* TODO: chips with no XIP use should ignore and return */
	(void) map_read(map, adr); /* ensure mmu mapping is up to date */
	preempt_disable();
#ifdef CONFIG_ILATENCY
	__intr_cli();
#else
	local_irq_disable();
#endif
}

static void __xipram xip_enable(struct map_info *map, struct flchip *chip,
				unsigned long adr)
{
	struct cfi_private *cfi = map->fldrv_priv;
	if (chip->state != FL_POINT && chip->state != FL_READY) {
		map_write(map, CMD(0xff), adr);
		chip->state = FL_READY;
	}
	(void) map_read(map, adr);
	asm volatile (".rep 8; nop; .endr"); /* fill instruction prefetch */
#ifdef CONFIG_ILATENCY
	__intr_sti();
#else
	local_irq_enable();
#endif
	preempt_enable();
}

/*
 * When a delay is required for the flash operation to complete, the
 * xip_udelay() function is polling for both the given timeout and pending
 * (but still masked) hardware interrupts.  Whenever there is an interrupt
 * pending then the flash erase or write operation is suspended, array mode
 * restored and interrupts unmasked.  Task scheduling might also happen at that
 * point.  The CPU eventually returns from the interrupt or the call to
 * schedule() and the suspended flash operation is resumed for the remaining
 * of the delay period.
 *
 * Warning: this function _will_ fool interrupt latency tracing tools.
 */

static void __xipram xip_udelay(struct map_info *map, struct flchip *chip,
				unsigned long adr, int usec)
{
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_intelext *cfip = cfi->cmdset_priv;
	map_word status, OK = CMD(0x80);
	unsigned long suspended, start = xip_currtime();
	flstate_t oldstate, newstate;

	do {
		cpu_relax();
		if (xip_irqpending() && cfip &&
		    ((chip->state == FL_ERASING && (cfip->FeatureSupport&2)) ||
		     (chip->state == FL_WRITING && (cfip->FeatureSupport&4))) &&
		    (cfi_interleave_is_1(cfi) || chip->oldstate == FL_READY)) {
			/*
			 * Let's suspend the erase or write operation when
			 * supported.  Note that we currently don't try to
			 * suspend interleaved chips if there is already
			 * another operation suspended (imagine what happens
			 * when one chip was already done with the current
			 * operation while another chip suspended it, then
			 * we resume the whole thing at once).  Yes, it
			 * can happen!
			 */
			map_write(map, CMD(0xb0), adr);
			map_write(map, CMD(0x70), adr);
			usec -= xip_elapsed_since(start);
			suspended = xip_currtime();
			do {
				if (xip_elapsed_since(suspended) > 100000) {
					/*
					 * The chip doesn't want to suspend
					 * after waiting for 100 msecs.
					 * This is a critical error but there
					 * is not much we can do here.
					 */
					return;
				}
				status = map_read(map, adr);
			} while (!map_word_andequal(map, status, OK, OK));

			/* Suspend succeeded */
			oldstate = chip->state;
			if (oldstate == FL_ERASING) {
				if (!map_word_bitsset(map, status, CMD(0x40)))
					break;
				newstate = FL_XIP_WHILE_ERASING;
				chip->erase_suspended = 1;
			} else {
				if (!map_word_bitsset(map, status, CMD(0x04)))
					break;
				newstate = FL_XIP_WHILE_WRITING;
				chip->write_suspended = 1;
			}
			chip->state = newstate;
			map_write(map, CMD(0xff), adr);
			(void) map_read(map, adr);
			asm volatile (".rep 8; nop; .endr");
#ifdef CONFIG_ILATENCY
			__intr_sti();
#else
			local_irq_enable();
#endif
			preempt_enable();
			asm volatile (".rep 8; nop; .endr");
			cond_resched();

			/*
			 * We're back.  However someone else might have
			 * decided to go write to the chip if we are in
			 * a suspended erase state.  If so let's wait
			 * until it's done.
			 */
			preempt_disable();
			while (chip->state != newstate) {
				DECLARE_WAITQUEUE(wait, current);
				set_current_state(TASK_UNINTERRUPTIBLE);
				add_wait_queue(&chip->wq, &wait);
				preempt_enable();
				schedule();
				remove_wait_queue(&chip->wq, &wait);
				preempt_disable();
			}
			/* Disallow XIP again */
#ifdef CONFIG_ILATENCY
			__intr_cli();
#else
			local_irq_disable();
#endif

			/* Resume the write or erase operation */
			map_write(map, CMD(0xd0), adr);
			map_write(map, CMD(0x70), adr);
			chip->state = oldstate;
			start = xip_currtime();
		} else if (usec >= 1000000/HZ) {
			/*
			 * Try to save on CPU power when waiting delay
			 * is at least a system timer tick period.
			 * No need to be extremely accurate here.
			 */
			xip_cpu_idle();
		}
		status = map_read(map, adr);
	} while (!map_word_andequal(map, status, OK, OK)
		 && xip_elapsed_since(start) < usec);
}

#define UDELAY(map, chip, adr, usec)  xip_udelay(map, chip, adr, usec)

/*
 * The INVALIDATE_CACHED_RANGE() macro is normally used in parallel while
 * the flash is actively programming or erasing since we have to poll for
 * the operation to complete anyway.  We can't do that in a generic way with
 * a XIP setup so do it before the actual flash operation in this case.
 */
#undef INVALIDATE_CACHED_RANGE
#define INVALIDATE_CACHED_RANGE(x...)
#define XIP_INVAL_CACHED_RANGE(map, from, size) \
	do { if(map->inval_cache) map->inval_cache(map, from, size); } while(0)

/*
 * Extra notes:
 *
 * Activating this XIP support changes the way the code works a bit.  For
 * example the code to suspend the current process when concurrent access
 * happens is never executed because xip_udelay() will always return with the
 * same chip state as it was entered with.  This is why there is no care for
 * the presence of add_wait_queue() or schedule() calls from within a couple
 * xip_disable()'d  areas of code, like in do_erase_oneblock for example.
 * The queueing and scheduling are always happening within xip_udelay().
 *
 * Similarly, get_chip() and put_chip() just happen to always be executed
 * with chip->state set to FL_READY (or FL_XIP_WHILE_*) where flash state
 * is in array mode, therefore never executing many cases therein and not
 * causing any problem with XIP.
 */

#else

#define xip_disable(map, chip, adr)
#define xip_enable(map, chip, adr)

#define UDELAY(map, chip, adr, usec)  cfi_udelay(usec)

#define XIP_INVAL_CACHED_RANGE(x...)

#endif

static int do_point_onechip (struct map_info *map, struct flchip *chip, loff_t adr, size_t len)
{
	unsigned long cmd_addr;
	struct cfi_private *cfi = map->fldrv_priv;
	int ret = 0;

	adr += chip->start;

	/* Ensure cmd read/writes are aligned. */ 
	cmd_addr = adr & ~(map_bankwidth(map)-1); 

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	spin_lock(chip->mutex);

	ret = get_chip(map, chip, cmd_addr, FL_POINT);

	if (!ret) {
		if (chip->state != FL_POINT && chip->state != FL_READY)
			map_write(map, CMD(0xff), cmd_addr);

		chip->state = FL_POINT;
		chip->ref_point_counter++;
	}
	spin_unlock(chip->mutex);
	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif
	return ret;
}

static int cfi_intelext_point (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char **mtdbuf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long ofs;
	int chipnum;
	int ret = 0;

	if (!map->virt || (from + len > mtd->size))
		return -EINVAL;
	
	*mtdbuf = (void *)map->virt + from;
	*retlen = 0;

	/* Now lock the chip(s) to POINT state */

	/* ofs: offset within the first chip that the first read should start */
	chipnum = (from >> cfi->chipshift);
	ofs = from - (chipnum << cfi->chipshift);

	while (len) {
		unsigned long thislen;

		if (chipnum >= cfi->numchips)
			break;

		if ((len + ofs -1) >> cfi->chipshift)
			thislen = (1<<cfi->chipshift) - ofs;
		else
			thislen = len;

		ret = do_point_onechip(map, &cfi->chips[chipnum], ofs, thislen);
		if (ret)
			break;

		*retlen += thislen;
		len -= thislen;
		
		ofs = 0;
		chipnum++;
	}
	return 0;
}

static void cfi_intelext_unpoint (struct mtd_info *mtd, u_char *addr, loff_t from, size_t len)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long ofs;
	int chipnum;

	/* Now unlock the chip(s) POINT state */

	/* ofs: offset within the first chip that the first read should start */
	chipnum = (from >> cfi->chipshift);
	ofs = from - (chipnum <<  cfi->chipshift);

	while (len) {
		unsigned long thislen;
		struct flchip *chip;

		chip = &cfi->chips[chipnum];
		if (chipnum >= cfi->numchips)
			break;

		if ((len + ofs -1) >> cfi->chipshift)
			thislen = (1<<cfi->chipshift) - ofs;
		else
			thislen = len;

		spin_lock(chip->mutex);
		if (chip->state == FL_POINT) {
			chip->ref_point_counter--;
			if(chip->ref_point_counter == 0)
				chip->state = FL_READY;
		} else
			printk(KERN_ERR "Warning: unpoint called on non pointed region\n"); /* Should this give an error? */

		put_chip(map, chip, chip->start);
		spin_unlock(chip->mutex);

		len -= thislen;
		ofs = 0;
		chipnum++;
	}
}

static inline int do_read_onechip(struct map_info *map, struct flchip *chip, loff_t adr, size_t len, u_char *buf)
{
	unsigned long cmd_addr;
	struct cfi_private *cfi = map->fldrv_priv;
	int ret;

	adr += chip->start;

	/* Ensure cmd read/writes are aligned. */ 
	cmd_addr = adr & ~(map_bankwidth(map)-1); 

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	spin_lock(chip->mutex);
	ret = get_chip(map, chip, cmd_addr, FL_READY);
	if (ret) {
		spin_unlock(chip->mutex);
		#ifdef CONFIG_PM
			atomic_dec(&(pm_flash.pm_flash_count));
		#endif		
		return ret;
	}

	if (chip->state != FL_POINT && chip->state != FL_READY) {
		map_write(map, CMD(0xff), cmd_addr);

		chip->state = FL_READY;
	}

	map_copy_from(map, buf, adr, len);

	put_chip(map, chip, cmd_addr);

	spin_unlock(chip->mutex);
	
	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif	
	return 0;
}

static int cfi_intelext_read (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long ofs;
	int chipnum;
	int ret = 0;

	/* ofs: offset within the first chip that the first read should start */
	chipnum = (from >> cfi->chipshift);
	ofs = from - (chipnum <<  cfi->chipshift);

	*retlen = 0;

	while (len) {
		unsigned long thislen;

		if (chipnum >= cfi->numchips)
			break;

		if ((len + ofs -1) >> cfi->chipshift)
			thislen = (1<<cfi->chipshift) - ofs;
		else
			thislen = len;

		ret = do_read_onechip(map, &cfi->chips[chipnum], ofs, thislen, buf);
		if (ret)
			break;

		*retlen += thislen;
		len -= thislen;
		buf += thislen;
		
		ofs = 0;
		chipnum++;
	}
	return ret;
}

static int __xipram do_write_oneword(struct map_info *map, struct flchip *chip,
				     unsigned long adr, map_word datum, int mode)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK, write_cmd;
	unsigned long timeo;
	int z, ret=0;

	adr += chip->start;

	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);
	switch (mode) {
	case FL_WRITING:   write_cmd = CMD(0x40); break;
	case FL_OTP_WRITE: write_cmd = CMD(0xc0); break;
	default: return -EINVAL;
	}

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	spin_lock(chip->mutex);
	ret = get_chip(map, chip, adr, mode);
	if (ret) {
		spin_unlock(chip->mutex);
		#ifdef CONFIG_PM
			atomic_dec(&(pm_flash.pm_flash_count));
		#endif		
		return ret;
	}

	XIP_INVAL_CACHED_RANGE(map, adr, map_bankwidth(map));
	ENABLE_VPP(map);
	xip_disable(map, chip, adr);
	map_write(map, write_cmd, adr);
	map_write(map, datum, adr);
	chip->state = mode;

	spin_unlock(chip->mutex);
	INVALIDATE_CACHED_RANGE(map, adr, map_bankwidth(map));
	UDELAY(map, chip, adr, chip->word_write_time);
	spin_lock(chip->mutex);

	timeo = jiffies + (HZ/2);
	z = 0;
	for (;;) {
		if (chip->state != mode) {
			/* Someone's suspended the write. Sleep */
			DECLARE_WAITQUEUE(wait, current);

			set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&chip->wq, &wait);
			spin_unlock(chip->mutex);
			schedule();
			remove_wait_queue(&chip->wq, &wait);
			timeo = jiffies + (HZ / 2); /* FIXME */
			spin_lock(chip->mutex);
			continue;
		}

		status = map_read(map, adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;
		
		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			chip->state = FL_STATUS;
			xip_enable(map, chip, adr);
			printk(KERN_ERR "waiting for chip to be ready timed out in word write\n");
			ret = -EIO;
			goto out;
		}

		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		z++;
		UDELAY(map, chip, adr, 1);
		spin_lock(chip->mutex);
	}
	if (!z) {
		chip->word_write_time--;
		if (!chip->word_write_time)
			chip->word_write_time++;
	}
	if (z > 1) 
		chip->word_write_time++;

	/* Done and happy. */
	chip->state = FL_STATUS;

	/* check for lock bit */
	if (map_word_bitsset(map, status, CMD(0x02))) {
		/* clear status */
		map_write(map, CMD(0x50), adr);
		/* put back into read status register mode */
		map_write(map, CMD(0x70), adr);
		ret = -EROFS;
	}

	xip_enable(map, chip, adr);
 out:	put_chip(map, chip, adr);
	spin_unlock(chip->mutex);
	
	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif
	return ret;
}

static int cfi_intelext_write_words (struct mtd_info *mtd, loff_t to , size_t len, size_t *retlen, const u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int ret = 0;
	int chipnum;
	unsigned long ofs;

	*retlen = 0;
	if (!len)
		return 0;

	chipnum = to >> cfi->chipshift;
	ofs = to  - (chipnum << cfi->chipshift);

	/* If it's not bus-aligned, do the first byte write */
	if (ofs & (map_bankwidth(map)-1)) {
		unsigned long bus_ofs = ofs & ~(map_bankwidth(map)-1);
		int gap = ofs - bus_ofs;
		int n;
		map_word datum;

		n = min_t(int, len, map_bankwidth(map)-gap);
		datum = map_word_ff(map);
		datum = map_word_load_partial(map, datum, buf, gap, n);

		ret = do_write_oneword(map, &cfi->chips[chipnum],
					       bus_ofs, datum, FL_WRITING);
		if (ret) 
			return ret;

		len -= n;
		ofs += n;
		buf += n;
		(*retlen) += n;

		if (ofs >> cfi->chipshift) {
			chipnum ++; 
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}
	
	while(len >= map_bankwidth(map)) {
		map_word datum = map_word_load(map, buf);

		ret = do_write_oneword(map, &cfi->chips[chipnum],
				       ofs, datum, FL_WRITING);
		if (ret)
			return ret;

		ofs += map_bankwidth(map);
		buf += map_bankwidth(map);
		(*retlen) += map_bankwidth(map);
		len -= map_bankwidth(map);

		if (ofs >> cfi->chipshift) {
			chipnum ++; 
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}

	if (len & (map_bankwidth(map)-1)) {
		map_word datum;

		datum = map_word_ff(map);
		datum = map_word_load_partial(map, datum, buf, 0, len);

		ret = do_write_oneword(map, &cfi->chips[chipnum],
				       ofs, datum, FL_WRITING);
		if (ret) 
			return ret;
		
		(*retlen) += len;
	}

	return 0;
}


static int __xipram do_write_buffer(struct map_info *map, struct flchip *chip, 
				    unsigned long adr, const u_char *buf, int len)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK;
	unsigned long cmd_adr, timeo;
	int wbufsize, z, ret=0, bytes, words;

	wbufsize = cfi_interleave(cfi) << cfi->cfiq->MaxBufWriteSize;
	adr += chip->start;
	cmd_adr = adr & ~(wbufsize-1);
	
	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	
	spin_lock(chip->mutex);
	ret = get_chip(map, chip, cmd_adr, FL_WRITING);
	if (ret) {
		spin_unlock(chip->mutex);
		#ifdef CONFIG_PM
			atomic_dec(&(pm_flash.pm_flash_count));
		#endif		
		return ret;
	}

	XIP_INVAL_CACHED_RANGE(map, adr, len);
	ENABLE_VPP(map);
	xip_disable(map, chip, cmd_adr);

	/* ?.8 of the 28FxxxJ3A datasheet says "Any time SR.4 and/or SR.5 is set
	   [...], the device will not accept any more Write to Buffer commands". 
	   So we must check here and reset those bits if they're set. Otherwise
	   we're just pissing in the wind */
	if (chip->state != FL_STATUS)
		map_write(map, CMD(0x70), cmd_adr);
	status = map_read(map, cmd_adr);
	if (map_word_bitsset(map, status, CMD(0x30))) {
		xip_enable(map, chip, cmd_adr);
		printk(KERN_WARNING "SR.4 or SR.5 bits set in buffer write (status %lx). Clearing.\n", status.x[0]);
		xip_disable(map, chip, cmd_adr);
		map_write(map, CMD(0x50), cmd_adr);
		map_write(map, CMD(0x70), cmd_adr);
	}

	chip->state = FL_WRITING_TO_BUFFER;

	z = 0;
	for (;;) {
		map_write(map, CMD(0xe8), cmd_adr);

		status = map_read(map, cmd_adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;

		spin_unlock(chip->mutex);
		UDELAY(map, chip, cmd_adr, 1);
		spin_lock(chip->mutex);

		if (++z > 20) {
			/* Argh. Not ready for write to buffer */
			map_word Xstatus;
			map_write(map, CMD(0x70), cmd_adr);
			chip->state = FL_STATUS;
			Xstatus = map_read(map, cmd_adr);
			/* Odd. Clear status bits */
			map_write(map, CMD(0x50), cmd_adr);
			map_write(map, CMD(0x70), cmd_adr);
			xip_enable(map, chip, cmd_adr);
			printk(KERN_ERR "Chip not ready for buffer write. status = %lx, Xstatus = %lx\n",
			       status.x[0], Xstatus.x[0]);
			ret = -EIO;
			goto out;
		}
	}

	/* Write length of data to come */
	bytes = len & (map_bankwidth(map)-1);
	words = len / map_bankwidth(map);
	map_write(map, CMD(words - !bytes), cmd_adr );

	/* Write data */
	z = 0;
	while(z < words * map_bankwidth(map)) {
		map_word datum = map_word_load(map, buf);
		map_write(map, datum, adr+z);

		z += map_bankwidth(map);
		buf += map_bankwidth(map);
	}

	if (bytes) {
		map_word datum;

		datum = map_word_ff(map);
		datum = map_word_load_partial(map, datum, buf, 0, bytes);
		map_write(map, datum, adr+z);
	}

	/* GO GO GO */
	map_write(map, CMD(0xd0), cmd_adr);
	chip->state = FL_WRITING;

	spin_unlock(chip->mutex);
	INVALIDATE_CACHED_RANGE(map, adr, len);
	UDELAY(map, chip, cmd_adr, chip->buffer_write_time);
	spin_lock(chip->mutex);

	timeo = jiffies + (HZ/2);
	z = 0;
	for (;;) {
		if (chip->state != FL_WRITING) {
			/* Someone's suspended the write. Sleep */
			DECLARE_WAITQUEUE(wait, current);
			set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&chip->wq, &wait);
			spin_unlock(chip->mutex);
			schedule();
			remove_wait_queue(&chip->wq, &wait);
			timeo = jiffies + (HZ / 2); /* FIXME */
			spin_lock(chip->mutex);
			continue;
		}

		status = map_read(map, cmd_adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;

		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			chip->state = FL_STATUS;
			xip_enable(map, chip, cmd_adr);
			printk(KERN_ERR "waiting for chip to be ready timed out in bufwrite\n");
			ret = -EIO;
			goto out;
		}
		
		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		UDELAY(map, chip, cmd_adr, 1);
		z++;
		spin_lock(chip->mutex);
	}
	if (!z) {
		chip->buffer_write_time--;
		if (!chip->buffer_write_time)
			chip->buffer_write_time++;
	}
	if (z > 1) 
		chip->buffer_write_time++;

	/* Done and happy. */
 	chip->state = FL_STATUS;

	/* check for lock bit */
	if (map_word_bitsset(map, status, CMD(0x02))) {
		/* clear status */
		map_write(map, CMD(0x50), cmd_adr);
		/* put back into read status register mode */
		map_write(map, CMD(0x70), adr);
		ret = -EROFS;
	}

	xip_enable(map, chip, cmd_adr);
 out:	put_chip(map, chip, cmd_adr);
	spin_unlock(chip->mutex);
	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif	
	return ret;
}

static int cfi_intelext_write_buffers (struct mtd_info *mtd, loff_t to, 
				       size_t len, size_t *retlen, const u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int wbufsize = cfi_interleave(cfi) << cfi->cfiq->MaxBufWriteSize;
	int ret = 0;
	int chipnum;
	unsigned long ofs;

	*retlen = 0;
	if (!len)
		return 0;

	chipnum = to >> cfi->chipshift;
	ofs = to  - (chipnum << cfi->chipshift);

	/* If it's not bus-aligned, do the first word write */
	if (ofs & (map_bankwidth(map)-1)) {
		size_t local_len = (-ofs)&(map_bankwidth(map)-1);
		if (local_len > len)
			local_len = len;
		ret = cfi_intelext_write_words(mtd, to, local_len,
					       retlen, buf);
		if (ret)
			return ret;
		ofs += local_len;
		buf += local_len;
		len -= local_len;

		if (ofs >> cfi->chipshift) {
			chipnum ++;
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}

	while(len) {
		/* We must not cross write block boundaries */
		int size = wbufsize - (ofs & (wbufsize-1));

		if (size > len)
			size = len;
		ret = do_write_buffer(map, &cfi->chips[chipnum], 
				      ofs, buf, size);
		if (ret)
			return ret;

		ofs += size;
		buf += size;
		(*retlen) += size;
		len -= size;

		if (ofs >> cfi->chipshift) {
			chipnum ++; 
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}
	return 0;
}

static int __xipram do_erase_oneblock(struct map_info *map, struct flchip *chip,
				      unsigned long adr, int len, void *thunk)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK;
	unsigned long timeo;
	int retries = 3;
	DECLARE_WAITQUEUE(wait, current);
	int ret = 0;

	adr += chip->start;

	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
 retry:
	spin_lock(chip->mutex);
	ret = get_chip(map, chip, adr, FL_ERASING);
	if (ret) {
		spin_unlock(chip->mutex);
		#ifdef CONFIG_PM
			atomic_dec(&(pm_flash.pm_flash_count));
		#endif
		return ret;
	}

	XIP_INVAL_CACHED_RANGE(map, adr, len);
	ENABLE_VPP(map);
	xip_disable(map, chip, adr);

	/* Clear the status register first */
	map_write(map, CMD(0x50), adr);

	/* Now erase */
	map_write(map, CMD(0x20), adr);
	map_write(map, CMD(0xD0), adr);
	chip->state = FL_ERASING;
	chip->erase_suspended = 0;

	spin_unlock(chip->mutex);
	INVALIDATE_CACHED_RANGE(map, adr, len);
	UDELAY(map, chip, adr, chip->erase_time*1000/2);
	spin_lock(chip->mutex);

	/* FIXME. Use a timer to check this, and return immediately. */
	/* Once the state machine's known to be working I'll do that */

	timeo = jiffies + (HZ*20);
	for (;;) {
		if (chip->state != FL_ERASING) {
			/* Someone's suspended the erase. Sleep */
			set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&chip->wq, &wait);
			spin_unlock(chip->mutex);
			schedule();
			remove_wait_queue(&chip->wq, &wait);
			spin_lock(chip->mutex);
			continue;
		}
		if (chip->erase_suspended) {
			/* This erase was suspended and resumed.
			   Adjust the timeout */
			timeo = jiffies + (HZ*20); /* FIXME */
			chip->erase_suspended = 0;
		}

		status = map_read(map, adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;
		
		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			map_word Xstatus;
			map_write(map, CMD(0x70), adr);
			chip->state = FL_STATUS;
			Xstatus = map_read(map, adr);
			/* Clear status bits */
			map_write(map, CMD(0x50), adr);
			map_write(map, CMD(0x70), adr);
			xip_enable(map, chip, adr);
			printk(KERN_ERR "waiting for erase at %08lx to complete timed out. status = %lx, Xstatus = %lx.\n",
			       adr, status.x[0], Xstatus.x[0]);
			ret = -EIO;
			goto out;
		}
		
		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		UDELAY(map, chip, adr, 1000000/HZ);
		spin_lock(chip->mutex);
	}

	/* We've broken this before. It doesn't hurt to be safe */
	map_write(map, CMD(0x70), adr);
	chip->state = FL_STATUS;
	status = map_read(map, adr);

	/* check for lock bit */
	if (map_word_bitsset(map, status, CMD(0x3a))) {
		unsigned char chipstatus;

		/* Reset the error bits */
		map_write(map, CMD(0x50), adr);
		map_write(map, CMD(0x70), adr);
		xip_enable(map, chip, adr);

		chipstatus = status.x[0];
		if (!map_word_equal(map, status, CMD(chipstatus))) {
			int i, w;
			for (w=0; w<map_words(map); w++) {
				for (i = 0; i<cfi_interleave(cfi); i++) {
					chipstatus |= status.x[w] >> (cfi->device_type * 8);
				}
			}
			printk(KERN_WARNING "Status is not identical for all chips: 0x%lx. Merging to give 0x%02x\n",
			       status.x[0], chipstatus);
		}

		if ((chipstatus & 0x30) == 0x30) {
			printk(KERN_NOTICE "Chip reports improper command sequence: status 0x%x\n", chipstatus);
			ret = -EIO;
		} else if (chipstatus & 0x02) {
			/* Protection bit set */
			ret = -EROFS;
		} else if (chipstatus & 0x8) {
			/* Voltage */
			printk(KERN_WARNING "Chip reports voltage low on erase: status 0x%x\n", chipstatus);
			ret = -EIO;
		} else if (chipstatus & 0x20) {
			if (retries--) {
				printk(KERN_DEBUG "Chip erase failed at 0x%08lx: status 0x%x. Retrying...\n", adr, chipstatus);
				timeo = jiffies + HZ;
				put_chip(map, chip, adr);
				spin_unlock(chip->mutex);
				goto retry;
			}
			printk(KERN_DEBUG "Chip erase failed at 0x%08lx: status 0x%x\n", adr, chipstatus);
			ret = -EIO;
		}
	} else {
		xip_enable(map, chip, adr);
		ret = 0;
	}

 out:	put_chip(map, chip, adr);
	spin_unlock(chip->mutex);
	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif	
	return ret;
}

int cfi_intelext_erase_varsize(struct mtd_info *mtd, struct erase_info *instr)
{
	unsigned long ofs, len;
	int ret;

	ofs = instr->addr;
	len = instr->len;

	ret = cfi_varsize_frob(mtd, do_erase_oneblock, ofs, len, NULL);
	if (ret)
		return ret;

	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);
	
	return 0;
}

static void cfi_intelext_sync (struct mtd_info *mtd)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int i;
	struct flchip *chip;
	int ret = 0;

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	for (i=0; !ret && i<cfi->numchips; i++) {
		chip = &cfi->chips[i];

		spin_lock(chip->mutex);
		ret = get_chip(map, chip, chip->start, FL_SYNCING);

		if (!ret) {
			chip->oldstate = chip->state;
			chip->state = FL_SYNCING;
			/* No need to wake_up() on this state change - 
			 * as the whole point is that nobody can do anything
			 * with the chip now anyway.
			 */
		}
		spin_unlock(chip->mutex);
	}

	/* Unlock the chips again */

	for (i--; i >=0; i--) {
		chip = &cfi->chips[i];

		spin_lock(chip->mutex);
		
		if (chip->state == FL_SYNCING) {
			chip->state = chip->oldstate;
			chip->oldstate = FL_READY;
			wake_up(&chip->wq);
		}
		spin_unlock(chip->mutex);
	}
	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif
}

#ifdef DEBUG_LOCK_BITS
static int __xipram do_printlockstatus_oneblock(struct map_info *map,
						struct flchip *chip,
						unsigned long adr,
						int len, void *thunk)
{
	struct cfi_private *cfi = map->fldrv_priv;
	int status, ofs_factor = cfi->interleave * cfi->device_type;

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	xip_disable(map, chip, adr+(2*ofs_factor));
	cfi_send_gen_cmd(0x90, 0x55, 0, map, cfi, cfi->device_type, NULL);
	chip->state = FL_JEDEC_QUERY;
	status = cfi_read_query(map, adr+(2*ofs_factor));
	xip_enable(map, chip, 0);
	printk(KERN_DEBUG "block status register for 0x%08lx is %x\n",
	       adr, status);
	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif	       
	return 0;
}
#endif

#define DO_XXLOCK_ONEBLOCK_LOCK		((void *) 1)
#define DO_XXLOCK_ONEBLOCK_UNLOCK	((void *) 2)

static int __xipram do_xxlock_oneblock(struct map_info *map, struct flchip *chip,
				       unsigned long adr, int len, void *thunk)
{
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_intelext *extp = cfi->cmdset_priv;
	map_word status, status_OK;
	unsigned long timeo = jiffies + HZ;
	int ret;

	adr += chip->start;

	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	spin_lock(chip->mutex);
	ret = get_chip(map, chip, adr, FL_LOCKING);
	if (ret) {
		spin_unlock(chip->mutex);
		#ifdef CONFIG_PM
			atomic_dec(&(pm_flash.pm_flash_count));
		#endif		
		return ret;
	}

	ENABLE_VPP(map);
	xip_disable(map, chip, adr);
	
	map_write(map, CMD(0x60), adr);
	if (thunk == DO_XXLOCK_ONEBLOCK_LOCK) {
		map_write(map, CMD(0x01), adr);
		chip->state = FL_LOCKING;
	} else if (thunk == DO_XXLOCK_ONEBLOCK_UNLOCK) {
		map_write(map, CMD(0xD0), adr);
		chip->state = FL_UNLOCKING;
	} else
		BUG();

	/*
	 * If Instant Individual Block Locking supported then no need
	 * to delay.
	 */

	if (!extp || !(extp->FeatureSupport & (1 << 5))) {
		spin_unlock(chip->mutex);
		UDELAY(map, chip, adr, 1000000/HZ);
		spin_lock(chip->mutex);
	}

	/* FIXME. Use a timer to check this, and return immediately. */
	/* Once the state machine's known to be working I'll do that */

	timeo = jiffies + (HZ*20);
	for (;;) {

		status = map_read(map, adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;
		
		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			map_word Xstatus;
			map_write(map, CMD(0x70), adr);
			chip->state = FL_STATUS;
			Xstatus = map_read(map, adr);
			xip_enable(map, chip, adr);
			printk(KERN_ERR "waiting for unlock to complete timed out. status = %lx, Xstatus = %lx.\n",
			       status.x[0], Xstatus.x[0]);
			put_chip(map, chip, adr);
			spin_unlock(chip->mutex);
			
			#ifdef CONFIG_PM
				atomic_dec(&(pm_flash.pm_flash_count));
			#endif			
			return -EIO;
		}
		
		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		UDELAY(map, chip, adr, 1);
		spin_lock(chip->mutex);
	}
	
	/* Done and happy. */
	chip->state = FL_STATUS;
	xip_enable(map, chip, adr);
	put_chip(map, chip, adr);
	spin_unlock(chip->mutex);

	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif
	return 0;
}

static int cfi_intelext_lock(struct mtd_info *mtd, loff_t ofs, size_t len)
{
	int ret;

#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status before, ofs=0x%08llx, len=0x%08X\n",
	       __FUNCTION__, ofs, len);
	cfi_varsize_frob(mtd, do_printlockstatus_oneblock,
		ofs, len, 0);
#endif

	ret = cfi_varsize_frob(mtd, do_xxlock_oneblock, 
		ofs, len, DO_XXLOCK_ONEBLOCK_LOCK);
	
#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status after, ret=%d\n",
	       __FUNCTION__, ret);
	cfi_varsize_frob(mtd, do_printlockstatus_oneblock,
		ofs, len, 0);
#endif

	return ret;
}

static int cfi_intelext_unlock(struct mtd_info *mtd, loff_t ofs, size_t len)
{
	int ret;

#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status before, ofs=0x%08llx, len=0x%08X\n",
	       __FUNCTION__, ofs, len);
	cfi_varsize_frob(mtd, do_printlockstatus_oneblock,
		ofs, len, 0);
#endif

	ret = cfi_varsize_frob(mtd, do_xxlock_oneblock,
					ofs, len, DO_XXLOCK_ONEBLOCK_UNLOCK);
	
#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status after, ret=%d\n",
	       __FUNCTION__, ret);
	cfi_varsize_frob(mtd, do_printlockstatus_oneblock, 
		ofs, len, 0);
#endif
	
	return ret;
}

#if 1

typedef int (*otp_op_t)(struct map_info *map, struct flchip *chip, 
			u_long data_offset, u_char *buf, u_int size,
			u_long prot_offset, u_int groupno, u_int groupsize);

static int __xipram
do_otp_read(struct map_info *map, struct flchip *chip, u_long offset,
	    u_char *buf, u_int size, u_long prot, u_int grpno, u_int grpsz)
{
	struct cfi_private *cfi = map->fldrv_priv;
	int ret;

	#ifdef CONFIG_PM
		atomic_inc(&(pm_flash.pm_flash_count));
	#endif
	spin_lock(chip->mutex);
	ret = get_chip(map, chip, chip->start, FL_JEDEC_QUERY);
	if (ret) {
		spin_unlock(chip->mutex);
		#ifdef CONFIG_PM
			atomic_dec(&(pm_flash.pm_flash_count));
		#endif		
		return ret;
	}

	/* let's ensure we're not reading back cached data from array mode */
	if (map->inval_cache)
		map->inval_cache(map, chip->start + offset, size);

	xip_disable(map, chip, chip->start);
	if (chip->state != FL_JEDEC_QUERY) {
		map_write(map, CMD(0x90), chip->start);
		chip->state = FL_JEDEC_QUERY;
	}
	map_copy_from(map, buf, chip->start + offset, size);
	xip_enable(map, chip, chip->start);

	/* then ensure we don't keep OTP data in the cache */
	if (map->inval_cache)
		map->inval_cache(map, chip->start + offset, size);

	put_chip(map, chip, chip->start);
	spin_unlock(chip->mutex);

	#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
	#endif
	return 0;
}

static int
do_otp_write(struct map_info *map, struct flchip *chip, u_long offset,
	     u_char *buf, u_int size, u_long prot, u_int grpno, u_int grpsz)
{
	int ret;

	while (size) {
		unsigned long bus_ofs = offset & ~(map_bankwidth(map)-1);
		int gap = offset - bus_ofs;
		int n = min_t(int, size, map_bankwidth(map)-gap);
		map_word datum = map_word_ff(map);

		datum = map_word_load_partial(map, datum, buf, gap, n);
		ret = do_write_oneword(map, chip, bus_ofs, datum, FL_OTP_WRITE);
		if (ret) 
			return ret;

		offset += n;
		buf += n;
		size -= n;
	}

	return 0;
}

static int
do_otp_lock(struct map_info *map, struct flchip *chip, u_long offset,
	    u_char *buf, u_int size, u_long prot, u_int grpno, u_int grpsz)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word datum;

	/* make sure area matches group boundaries */
	if (size != grpsz)
		return -EXDEV;

	datum = map_word_ff(map);
	datum = map_word_clr(map, datum, CMD(1 << grpno));
	return do_write_oneword(map, chip, prot, datum, FL_OTP_WRITE);
}

static int cfi_intelext_otp_walk(struct mtd_info *mtd, loff_t from, size_t len,
				 size_t *retlen, u_char *buf,
				 otp_op_t action, int user_regs)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_intelext *extp = cfi->cmdset_priv;
	struct flchip *chip;
	struct cfi_intelext_otpinfo *otp;
	u_long devsize, reg_prot_offset, data_offset;
	u_int chip_num, chip_step, field, reg_fact_size, reg_user_size;
	u_int groups, groupno, groupsize, reg_fact_groups, reg_user_groups,start_chip = 0;
	int ret;

	*retlen = 0;

	/* Check that we actually have some OTP registers */
	if (!extp || !(extp->FeatureSupport & 64) || !extp->NumProtectionFields)
		return -ENODATA;

	/* we need real chips here not virtual ones */
	devsize = (1 << cfi->cfiq->DevSize) * cfi->interleave;
	chip_step = devsize >> cfi->chipshift;

	/* Check if we have chip with Parameter Partition located on the top of memory map */
	/* (e.g. Intel StrataFlash Wireless L18) */
	if ((0xff & cfi->mfr) == 0x89) {
	   switch (0xff & cfi->id) { 
	    case 0xb:
	    case 0xc:
	    case 0xd:
	     start_chip =  chip_step - 1;
	   }
	 }

#ifdef CONFIG_ARCH_EZXBASE
	/* OTP specific request for EZXBASE products, only the first chip is impacted */
	for (chip_num = start_chip; chip_num < 1; chip_num += chip_step) {
#else
	for (chip_num = start_chip; chip_num < cfi->numchips; chip_num += chip_step) {
#endif
		chip = &cfi->chips[chip_num];
		otp = (struct cfi_intelext_otpinfo *)&extp->extra[0];

		/* first OTP region */
		field = 0;
		reg_prot_offset = extp->ProtRegAddr;
		reg_fact_groups = 1;
		reg_fact_size = 1 << extp->FactProtRegSize;
		reg_user_groups = 1;
		reg_user_size = 1 << extp->UserProtRegSize;

		while (len > 0) {
			/* flash geometry fixup */
			data_offset = reg_prot_offset + 1;
			data_offset *= cfi->interleave * cfi->device_type;
			reg_prot_offset *= cfi->interleave * cfi->device_type;
			reg_fact_size *= cfi->interleave;
			reg_user_size *= cfi->interleave;

			if (user_regs) {
				groups = reg_user_groups;
				groupsize = reg_user_size;
				/* skip over factory reg area */
				groupno = reg_fact_groups;
				data_offset += reg_fact_groups * reg_fact_size;
			} else {
				groups = reg_fact_groups;
				groupsize = reg_fact_size;
				groupno = 0;
			}

			while (len > 0 && groups > 0) {
				if (!action) {
					/*
					 * Special case: if action is NULL
					 * we fill buf with otp_info records.
					 */
					struct otp_info *otpinfo;
					map_word lockword;
					len -= sizeof(struct otp_info);
					if (len <= 0)
						return -ENOSPC;
					ret = do_otp_read(map, chip,
							  reg_prot_offset,
							  (u_char *)&lockword,
							  map_bankwidth(map),
							  0, 0,  0);
					if (ret)
						return ret;
					otpinfo = (struct otp_info *)buf;
					otpinfo->start = from;
					otpinfo->length = groupsize;
					otpinfo->locked =
					   !map_word_bitsset(map, lockword,
							     CMD(1 << groupno));
					from += groupsize;
					buf += sizeof(*otpinfo);
					*retlen += sizeof(*otpinfo);
				} else if (from >= groupsize) {
					from -= groupsize;
					data_offset += groupsize;
				} else {
					int size = groupsize;
					data_offset += from;
					size -= from;
					from = 0;
					if (size > len)
						size = len;
					ret = action(map, chip, data_offset,
						     buf, size, reg_prot_offset,
						     groupno, groupsize);
					if (ret < 0)
						return ret;
					buf += size;
					len -= size;
					*retlen += size;
					data_offset += size;
				}
				groupno++;
				groups--;
			}

			/* next OTP region */
			if (++field == extp->NumProtectionFields)
				break;
			reg_prot_offset = otp->ProtRegAddr;
			reg_fact_groups = otp->FactGroups;
			reg_fact_size = 1 << otp->FactProtRegSize;
			reg_user_groups = otp->UserGroups;
			reg_user_size = 1 << otp->UserProtRegSize;
			otp++;
		}
	}

	return 0;
}

static int cfi_intelext_read_fact_prot_reg(struct mtd_info *mtd, loff_t from,
					   size_t len, size_t *retlen,
					    u_char *buf)
{
	return cfi_intelext_otp_walk(mtd, from, len, retlen,
				     buf, do_otp_read, 0);
}

static int cfi_intelext_read_user_prot_reg(struct mtd_info *mtd, loff_t from,
					   size_t len, size_t *retlen,
					    u_char *buf)
{
	return cfi_intelext_otp_walk(mtd, from, len, retlen,
				     buf, do_otp_read, 1);
}

static int cfi_intelext_write_user_prot_reg(struct mtd_info *mtd, loff_t from,
					    size_t len, size_t *retlen,
					     u_char *buf)
{
	return cfi_intelext_otp_walk(mtd, from, len, retlen,
				     buf, do_otp_write, 1);
}

static int cfi_intelext_lock_user_prot_reg(struct mtd_info *mtd,
					   loff_t from, size_t len)
{
	size_t retlen;
	return cfi_intelext_otp_walk(mtd, from, len, &retlen,
				     NULL, do_otp_lock, 1);
}

static int cfi_intelext_get_fact_prot_info(struct mtd_info *mtd, 
					   struct otp_info *buf, size_t len)
{
	size_t retlen;
	int ret;

	ret = cfi_intelext_otp_walk(mtd, 0, len, &retlen, (u_char *)buf, NULL, 0);
	return ret ? : retlen;
}

static int cfi_intelext_get_user_prot_info(struct mtd_info *mtd,
					   struct otp_info *buf, size_t len)
{
	size_t retlen;
	int ret;

	ret = cfi_intelext_otp_walk(mtd, 0, len, &retlen, (u_char *)buf, NULL, 1);
	return ret ? : retlen;
}

#endif

static int msc0_val, sxcnfg_val;
static int mode = 0;

static int cfi_intelext_suspend(struct mtd_info *mtd)
{
	int flags;
	int status = 0;
	struct map_info *map = NULL;
	unsigned short i,j;
	struct cfi_private *cfi = NULL;
	struct flchip *this_chip = NULL;
		
	map = mtd->priv;
	cfi = (struct cfi_private *)(map->fldrv_priv);

	/* return flash status indicating sleep available or inavailable */
	status = atomic_read(&(pm_flash.pm_flash_count));
	printk(KERN_NOTICE "flash device refer-count(%d) when try to sleep\n",status);
	
	if (!status) 
	{
		printk("switch to async mode\n");
		
		/* Resume two chips' state to be FL_READY state */
		for ( i = 0; i < cfi->numchips; i ++ )
		{
			this_chip = &(cfi->chips[i]);
	
			map_write(map, CMD(0xff), this_chip->start);  //Make sure this chip is in read-array mode//
	
			this_chip->state = FL_READY;
			this_chip->oldstate = FL_READY;
		}		

		local_irq_save(flags);

				
		msc0_val = MSC0;
		sxcnfg_val = SXCNFG;

		MSC0 = 0x7ff03AFA;

		SXCNFG = 0;
		MDREFR &= ~(MDREFR_K0DB2 | MDREFR_K0RUN | MDREFR_E0PIN);


		local_irq_restore(flags);

		mode = 1;
	}
	
	return status;
}
int cfi_intelext_unlockdown(struct mtd_info *mymtd);

static void cfi_intelext_resume(struct mtd_info *mtd)
{
	int flags;
	int status = 0;

	if (mode) 
	{
		printk("resume to Async mode\n");

		local_irq_save(flags);


		MSC0 = msc0_val;
		MDREFR |= (MDREFR_K0DB2 | MDREFR_K0RUN | MDREFR_E0PIN);
			
		printk(KERN_NOTICE "Unlock after reset -- begin(%ld)\n", jiffies);
		status = cfi_intelext_upon_resume(ezx_mymtd);
		printk(KERN_NOTICE "Unlock after reset -- end(%ld)\n", jiffies);
	
		if (status)
			printk(KERN_NOTICE "Unlock flash fail after reset flash.\n");

	
		local_irq_restore(flags);
		mode = 0;
	}
	
	return;
}

static void cfi_intelext_destroy(struct mtd_info *mtd)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	kfree(cfi->cmdset_priv);
	kfree(cfi->cfiq);
	kfree(cfi->chips[0].priv);
	kfree(cfi);
	kfree(mtd->eraseregions);
}

/* Added by Susan for Intel Flash UNLOCK/LOCKDOWN mechanism */
int cfi_intelext_unlockdown(struct mtd_info *mymtd)
{
	struct map_info *map = NULL;
	unsigned short i, j, k, s_k, lockdown;
	struct cfi_private *cfi = NULL;
	struct flchip *this_chip = NULL;
	
	__u32 timeout = TIME_OUT;
	map_word flash_status;
	map_word OK_status = CMD(0x80);

//Susan for pm
#ifdef CONFIG_PM
	atomic_inc(&(pm_flash.pm_flash_count));
#endif

	map = mymtd->priv;
	cfi = (struct cfi_private *)(map->fldrv_priv);

	#ifdef CONFIG_L18_DEBUG
	printk("Enter into <cfi_intelext_unlockdown_L18>\n");
	#endif

	map_write(map, CMD(0x70), 0);
	flash_status = map_read(map, 0);
	map_write(map, CMD(0xff), 0);
	
	if ( !map_word_andequal(map, flash_status, OK_status, OK_status))
	{
	#ifdef CONFIG_INTELFLASH_DEBUG
	printk("cfi_intelext_unlockdown: initialization status wrong\n");
	#endif
		map_write(map, CMD(0x50), 0);  /* Clear status register */
#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
#endif
		return -EINVAL;
	}

	s_k = 0;  //search from the first fl_rw_region //

	for ( i = 0; i < mymtd->numeraseregions; i ++ )
	{
		unsigned short ebnums;
		unsigned long the_ebsize = 0;
		unsigned long the_offset = 0;

		ebnums = mymtd->eraseregions[i].numblocks;
		the_ebsize = mymtd->eraseregions[i].erasesize;
		the_offset = mymtd->eraseregions[i].offset;

		for (j = 0; j < ebnums; j ++)
		{
			lockdown = 1;
			for (k = s_k; k < sizeof(fl_rw_array)/sizeof(struct fl_rw_region); k ++)
			{
				if ( (the_offset >= fl_rw_array[k].rw_start) && (the_offset < fl_rw_array[k].rw_end) )
				{
					lockdown = 0;
					if (the_offset + the_ebsize >= fl_rw_array[k].rw_end)
						s_k = k + 1;
					break;
				}
			}

			if ( lockdown == 0 )
			{
				/* unlock it */
				map_write(map, CMD(0x60), the_offset);
				map_write(map, CMD(0xD0), the_offset);

				while ( -- timeout )
				{
					map_write(map, CMD(0x70), the_offset);
					flash_status = map_read(map, the_offset);
					if (map_word_andequal(map, flash_status, OK_status, OK_status))
						break;
				}

				map_write(map, CMD(0xff), the_offset);  /* Force flash returns to read-array mode */
				if (!timeout)
				{
					#ifdef CONFIG_L18_DEBUG
					printk("cfi_intelext_unlockdown_L18: unlock failed at %x\n", the_offset);
					#endif
					map_write(map, CMD(0x50), the_offset);  /* Clear status register before we leave here*/
#ifdef CONFIG_PM
					atomic_dec(&(pm_flash.pm_flash_count));
#endif
					return -EINVAL;
				}
				#ifdef CONFIG_L18_DEBUG
				printk("cfi_intelext_unlockdown_L18: unlock suceed at %x\n", the_offset);
				#endif
			}
			else
			{
				/* lock down it */
				map_write(map, CMD(0x60), the_offset);
				map_write(map, CMD(0x2F), the_offset);

				while ( -- timeout )
				{
					map_write(map, CMD(0x70), the_offset);
					flash_status = map_read(map, the_offset);
					if (map_word_andequal(map, flash_status, OK_status, OK_status))
						break;
				}

				map_write(map, CMD(0xff), the_offset);  /* Force flash returns to read-array mode */
				if (!timeout)
				{
					#ifdef CONFIG_L18_DEBUG
					printk("cfi_intelext_unlockdown_L18: lockdown failed at %x\n", the_offset);
					#endif
					map_write(map, CMD(0x50), the_offset);  /* Clear status register before we leave here*/
#ifdef CONFIG_PM
					atomic_dec(&(pm_flash.pm_flash_count));
#endif
					return -EINVAL;
				}
				#ifdef CONFIG_L18_DEBUG
				printk("cfi_intelext_unlockdown_L18: lockdown succeed at %x\n", the_offset);
				#endif
			}
			the_offset += the_ebsize;
			timeout = TIME_OUT;
		}

		map_write(map, CMD(0x50), (the_offset - the_ebsize));  /* clear status register when we leave this region */
		map_write(map, CMD(0xFF), (the_offset - the_ebsize));  /* Make sure the related partition to be in read-array mode */
	}

	/* Resume this chip's state to be FL_READY state */
	for ( i = 0; i < cfi->numchips; i ++ )
	{
		this_chip = &(cfi->chips[i]);
		this_chip->state = FL_READY;
		this_chip->oldstate = FL_READY;
	}

#ifdef CONFIG_PM
	atomic_dec(&(pm_flash.pm_flash_count));
#endif
	return 0;
}

/* This function is special for dataflash in Barbados based hardware, no reset happens to dataflash upon resume from sleep */
int cfi_intelext_upon_resume(struct mtd_info *mymtd)
{
	struct map_info *map = NULL;
	unsigned short i, j, k, s_k, lockdown;
	struct cfi_private *cfi = NULL;
	struct flchip *this_chip = NULL;
	
	__u32 timeout = TIME_OUT;
	map_word flash_status;
	map_word OK_status = CMD(0x80);

//Susan for pm
#ifdef CONFIG_PM
	atomic_inc(&(pm_flash.pm_flash_count));
#endif

	map = mymtd->priv;
	cfi = (struct cfi_private *)(map->fldrv_priv);

	#ifdef CONFIG_L18_DEBUG
	printk("Enter into <cfi_intelext_upon_resume>\n");
	#endif

	map_write(map, CMD(0x70), 0);
	flash_status = map_read(map, 0);
	map_write(map, CMD(0xff), 0);
	
	if ( !map_word_andequal(map, flash_status, OK_status, OK_status))
	{
	#ifdef CONFIG_INTELFLASH_DEBUG
	printk("cfi_intelext_upon_resume: Upon resume, initialization status wrong\n");
	#endif
		map_write(map, CMD(0x50), 0);  /* Clear status register */
#ifdef CONFIG_PM
		atomic_dec(&(pm_flash.pm_flash_count));
#endif
		return -EINVAL;
	}

	/* Resume two chips' state to be FL_READY state */
	for ( i = 0; i < cfi->numchips; i ++ )
	{
		this_chip = &(cfi->chips[i]);

		map_write(map, CMD(0xff), this_chip->start);  //Make sure this chip is in read-array mode//

		this_chip->state = FL_READY;
		this_chip->oldstate = FL_READY;
	}

#ifdef CONFIG_PM
	atomic_dec(&(pm_flash.pm_flash_count));
#endif
	return 0;
}

int cfi_intelext_read_roflash(void *priv_map, unsigned long from, size_t len, size_t *retlen, u_char *buf)
{
	struct map_info *map = (struct map_info *)(priv_map);
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long ofs;
	int chipnum;
	int ret = 0;

	/* ofs: offset within the first chip that the first read should start */
	chipnum = (from >> cfi->chipshift);
	ofs = from - (chipnum <<  cfi->chipshift);

	*retlen = 0;

	while (len) {
		unsigned long thislen;

		if (chipnum >= cfi->numchips)
			break;

		if ((len + ofs -1) >> cfi->chipshift)
			thislen = (1<<cfi->chipshift) - ofs;
		else
			thislen = len;

		ret = do_read_onechip(map, &cfi->chips[chipnum], ofs, thislen, buf);
		if (ret)
			break;

		*retlen += thislen;
		len -= thislen;
		buf += thislen;
		
		ofs = 0;
		chipnum++;
	}
	return ret;
}


static char im_name_1[]="cfi_cmdset_0001";
static char im_name_3[]="cfi_cmdset_0003";

int __init cfi_intelext_init(void)
{
	inter_module_register(im_name_1, THIS_MODULE, &cfi_cmdset_0001);
	inter_module_register(im_name_3, THIS_MODULE, &cfi_cmdset_0001);
	
	/* Added by Susan for initialization of pm_flash device */
#ifdef CONFIG_PM
	atomic_set(&(pm_flash.pm_flash_count), 0);
#endif
	return 0;
}

static void __exit cfi_intelext_exit(void)
{
	inter_module_unregister(im_name_1);
	inter_module_unregister(im_name_3);
}

module_init(cfi_intelext_init);
module_exit(cfi_intelext_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org> et al.");
MODULE_DESCRIPTION("MTD chip driver for Intel/Sharp flash chips");
