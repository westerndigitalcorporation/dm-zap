// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 */

#include <linux/types.h>
#include <linux/blkdev.h>
#include "dm-zoned.h"

#define	DM_MSG_PREFIX		"zap"

#define DM_ZAP_MAGIC	0x72927048	/* "zap0" */
#define DM_ZAP_VERSION	1

#define DMZAP_UNMAPPED (~((sector_t) 0))

/*
 * dm-zap internal zone types
 */
enum {
	DMZAP_META,
	DMZAP_RAND,
};

/*
 * dm-zap managed zones descriptor.
 */
struct dmzap_zone {
	unsigned int		type;	/* DMZAP_ META/RAND */
	u32			seq;	/* Sequence id */
};

/* Zone metadata */

struct dmzap_zone_header {
	__le32			magic;
	__le32			version;
	__le32			seq;
	char			reserved[492];
	__le32			crc;
};

struct dmzap_zone_footer {
	__le32			seq;
	__le32			map_start;
	char			reserved[496];
	__le32			crc;
};

struct dmzap_map_entry {
	__le32			start;
	__le32			length;
	__le32			address;
};

struct dmzap_meta_zone {
	struct blk_zone		*zone;
};

struct dmzap_rand_zone {
	struct blk_zone		*zone;
};

struct dmzap_map {
	sector_t		*l2d;	/* Logical to device mapping */
};

enum {
	DMZAP_WR_OUTSTANDING,
};

/*
 * Target descriptor.
 */
struct dmzap_target {
	struct dm_dev		*ddev;

	/* dmzap block device information */
	struct dmz_dev		*dev;

	/* For cloned BIOs to conventional zones */
	struct bio_set		bio_set;

	/* User exposed capacity */
	unsigned int		nr_conv_zones;
	unsigned int		nr_seq_zones;
	sector_t		capacity;

	/* Conventional zones */
	struct blk_zone		*conv_zones;

	/* Internal geometry
	 *  dev->nr_zones = nr_meta_zones + nr_rand_zones + nr_sec_zones
	 */
	unsigned int		nr_meta_zones;
	unsigned int		nr_rand_zones;
	unsigned int		nr_internal_zones; /* rand + meta */

	sector_t		seq_zones_start;
	sector_t		seq_zones_offset;

	/* Internally managed zones */
	struct blk_zone		*internal_zones; /* Backing blockdevice zones*/

	struct dmzap_meta_zone	*meta_zones;
	struct dmzap_rand_zone	*rand_zones;

	struct dmzap_map	map;

	/* Write serialization for conventional zones */
	sector_t		rand_wp;	/* Next write on backing rand zones */
	unsigned long		write_bitmap;   /* Conventional zone write state bitmap */
};

/*
 * Zone BIO context.
 */
struct dmzap_bioctx {
	struct dmzap_target	*target;
	struct bio		*bio;
	refcount_t		ref;
	sector_t		user_sec;
};

int dmzap_map_init(struct dmzap_target *dmzap);
void dmzap_map_free(struct dmzap_target *dmzap);
int dmzap_map_update(struct dmzap_target *dmzap, sector_t user, sector_t backing,
	      sector_t len);
int dmzap_map_update_if_eq(struct dmzap_target *dmzap, sector_t user, sector_t backing,
	      sector_t len, sector_t *orig_secs);
int dmzap_map_lookup(struct dmzap_target *dmzap, sector_t user, sector_t *backing,
	      sector_t len);

int dmzap_map_conv(struct dmzap_target *dmzap, struct bio *bio);

