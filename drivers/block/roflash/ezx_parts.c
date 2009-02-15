/*
 * Copyright (C) 2002, 2005 Motorola Inc.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  History:
 *
 * drivers/mtd/maps/ezx_parts.c
 *
 * Parse multiple flash partitions in Ezx project into mtd_table or into act_roflash_table
 *
 * Created by Susan Gu  Oct, 22  2002
 *
 * Ru yi(e5537c)  Mar.10,2005           add linear roflash support
 * 
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/io.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>
#include <linux/ezx_roflash.h>

extern int roflash_init(void);

extern int cfi_intelext_read_roflash(void *priv_map, unsigned long from, size_t len, size_t *retlen, u_char *buf);
extern int cfi_intelext_read_roflash_c(void *priv_map, unsigned long from, size_t len, size_t *retlen, u_char *buf);
roflash_area **roflash_table_pptr = NULL;

int linear_roflash_read(void *priv_map, unsigned long from, size_t len, size_t *retlen, u_char *buf)
{
	unsigned char *map_offset = (unsigned char *)(priv_map + from);
	memcpy(buf, map_offset, len);
	*retlen = len;
	return 0;	
}

static DECLARE_MUTEX(roflash_table_mutex);

/* I know what I am doing, so setup l_x_b flag */

static roflash_area  fix_roflash_table[]= 
{
	{
		name:		"ROOT",
		size:		0x01720000,
		offset:		0x001A0000,
		l_x_b:		ROFLASH_LINEAR_XIP,
		phys_addr:	0x00000000,
                roflash_read:   NULL,
	},{
		name:		"OPT",  
		size:		0x02620000,
		offset:		0x019E0000,
		l_x_b:		ROFLASH_LINEAR_XIP,
		phys_addr:      0x02000000, 
                roflash_read:   NULL,
	},
};


roflash_area *act_roflash_table[MAX_ROFLASH] = {0};
unsigned short roflash_partitions = 0;

/* If we decide to use flash partition parse table, this function will parse the fpt - flash partition table,
*  the beginning address of fpt is defined in CONFIG_FPT_OFFSET.  This function will parser out all roflash areas
*  and animation area( or re-fresh code area).   
*  For roflash areas, registered them into act_roflash_table, for animation region, register it into mtd_table[6].
*  Note 
*  6 is fixed for animation region -- /dev/mtd6 c 90 12
*  /dev/roflash b 62 0  -- root file system
*  /dev/roflash1 b 62 1 -- language package
*  /dev/roflash2 b 62 2 -- setup stuff
*/  
int ezx_parse(struct map_info *chip_map)
{
	return 0;  // depends on discussion about DC tool
}

int ezx_parts_parser(void)
{
	int ret = 0;
	int nrparts = 0;
	int i = 0;

//#ifdef CONFIG_EZX_PARSER_TABLE
//	ret = ezx_parse(chip_map);
//#else
	down(&roflash_table_mutex);

	nrparts = sizeof(fix_roflash_table) / sizeof(roflash_area);

#ifdef ROFLASH_DEBUG_ERR
	printk("ezx_parts_parser:  nrparts is %d\n", nrparts);
#endif
	for ( i = 0; i < nrparts; i ++ )
	{
		if (act_roflash_table[i])
			return -1;
		act_roflash_table[i] = (roflash_area *)(&(fix_roflash_table[i]));
#ifdef ROFLASH_DEBUG_ERR
		printk(KERN_ERR "ezx_parts_parser: act_roflash_table[%d] = %d\n",i,(unsigned long)(act_roflash_table[i]));
#endif
	}
	roflash_partitions = i;

#ifdef ROFLASH_DEBUG_ERR
	printk("ezx_parts_parser:  roflash_partitions is %d\n", roflash_partitions);
#endif
	
	up(&roflash_table_mutex);
	
	return ret;
}
			
int ezx_partition_init(void)
{
	int ret = 0;

	
#ifdef ROFLASH_DEBUG_ERR
	printk(KERN_ERR "ezx_partition_init: roflash_table_pptr = %d\n",(unsigned long)(act_roflash_table));
#endif
	roflash_table_pptr = (roflash_area **)(act_roflash_table);

	ret = ezx_parts_parser();
	if ( ret )
	{
		printk(KERN_NOTICE "invoke ezx_parts_parser, return %d\n", ret);
		return -EIO;
	}

#ifdef ROFLASH_DEBUG_ERR
	printk("ezx_partition_init:  ret of ezx_parts_parser is %d\n", ret);
#endif

	ret = roflash_init();
	if ( ret )
	{
		printk(KERN_NOTICE "invoke roflash_init, return %d\n", ret);
		return -EIO;
	}
	return 0 ;
}






