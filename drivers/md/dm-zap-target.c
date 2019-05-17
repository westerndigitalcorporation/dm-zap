// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 */

#include "dm-zap.h"

#include <linux/module.h>

/* TODO: this might not work for us. Calculate the need */
#define DMZAP_MIN_BIOS		8192


/*
 * Cleanup zap device information.
 */
static void dmzap_put_device(struct dm_target *ti)
{
	struct dmzap_target *dmzap = ti->private;

	dm_put_device(ti, dmzap->ddev);
	kfree(dmzap->dev);
	dmzap->dev = NULL;
}

/*
 * Process a bio
 */
static int dmzap_map(struct dm_target *ti, struct bio *bio)
{
	struct dmzap_target *dmzap = ti->private;
	struct dmz_dev *dev = dmzap->dev;
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned int z;

	z = sector >> dev->zone_nr_sectors_shift;

	if (z < dmzap->nr_conv_zones)
		return dmzap_map_conv(dmzap, bio);

	bio_set_dev(bio, dev->bdev);

	dmz_dev_debug(dev, "remapping %llu sectors from %llu to %llu",
		(unsigned long long)bio_sectors(bio),
		(unsigned long long)sector,
		(unsigned long long)(sector + dmzap->seq_zones_offset));

	bio->bi_iter.bi_sector += dmzap->seq_zones_offset;

	return DM_MAPIO_REMAPPED;
}

static int dmzap_report_zones(struct dm_target *ti, sector_t sector,
			       struct blk_zone *zones, unsigned int *nr_zones,
			       gfp_t gfp_mask)
{
	struct dmzap_target *dmzap = ti->private;
	struct dmz_dev *dev = dmzap->dev;
	unsigned int nr_conv = 0;
	unsigned int nr_seq = 0;
	unsigned int z;

	z = sector >> dev->zone_nr_sectors_shift;

	/* Report conventional zones? */
	if (z < dmzap->nr_conv_zones) {
		nr_conv = min_t(unsigned int, *nr_zones,
				dmzap->nr_conv_zones - z);

		memcpy(zones, &dmzap->conv_zones[z],
				nr_conv * sizeof(struct blk_zone));

		z += nr_conv;
	}

	/* Report sequential zones? */
	if (nr_conv < *nr_zones) {
		z -= dmzap->nr_conv_zones;
		if (z < dmzap->nr_seq_zones) {
			nr_seq = min_t(unsigned int,
					*nr_zones - nr_conv,
					dmzap->nr_seq_zones - z);
		}
	}

	if (nr_seq) {
		sector_t seq_sector = dmzap->seq_zones_start;
		int ret;

		seq_sector += z << dev->zone_nr_sectors_shift;
		ret = blkdev_report_zones(dev->bdev, seq_sector,
				&zones[nr_conv], &nr_seq, gfp_mask);
		if (ret != 0)
			return ret;

		if (nr_seq) {
			dm_remap_zone_report(ti, dmzap->seq_zones_offset,
				     &zones[nr_conv], &nr_seq);
		}
	}

	*nr_zones = nr_conv + nr_seq;

	return 0;
}

/*
 * Get zoned device information. This is a copy of dm-zoned's
 * dmz_get_zoned_device
 * TODO: make common ?
 */
static int dmzap_get_zoned_device(struct dm_target *ti, char *path)
{
	struct dmzap_target *dmz = ti->private;
	struct request_queue *q;
	struct dmz_dev *dev;
	sector_t aligned_capacity;
	int ret;

	/* Get the target device */
	ret = dm_get_device(ti, path, dm_table_get_mode(ti->table), &dmz->ddev);
	if (ret) {
		ti->error = "Get target device failed";
		dmz->ddev = NULL;
		return ret;
	}

	dev = kzalloc(sizeof(struct dmz_dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto err;
	}

	dev->bdev = dmz->ddev->bdev;
	(void)bdevname(dev->bdev, dev->name);

	if (bdev_zoned_model(dev->bdev) == BLK_ZONED_NONE) {
		ti->error = "Not a zoned block device";
		ret = -EINVAL;
		goto err;
	}

	q = bdev_get_queue(dev->bdev);
	dev->capacity = i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT;
	aligned_capacity = dev->capacity & ~(blk_queue_zone_sectors(q) - 1);
	if (ti->begin ||
	    ((ti->len != dev->capacity) && (ti->len != aligned_capacity))) {
		ti->error = "Partial mapping not supported";
		ret = -EINVAL;
		goto err;
	}

	dev->zone_nr_sectors = blk_queue_zone_sectors(q);
	dev->zone_nr_sectors_shift = ilog2(dev->zone_nr_sectors);

	dev->zone_nr_blocks = dmz_sect2blk(dev->zone_nr_sectors);
	dev->zone_nr_blocks_shift = ilog2(dev->zone_nr_blocks);

	dev->nr_zones = blkdev_nr_zones(dev->bdev);

	dmz->dev = dev;

	return 0;
err:
	dm_put_device(ti, dmz->ddev);
	kfree(dev);

	return ret;
}

int dmzap_zones_init(struct dmzap_target *dmzap)
{
	struct dmz_dev *dev = dmzap->dev;
	unsigned int i, j, nr_zones;
	sector_t sector = 0;
	int ret;

	dmzap->conv_zones = kvmalloc_array(dmzap->nr_conv_zones,
			sizeof(struct blk_zone), GFP_KERNEL | __GFP_ZERO);
	if (!dmzap->conv_zones)
		return -ENOMEM;

	dmzap->internal_zones = kvmalloc_array(dmzap->nr_internal_zones,
			sizeof(struct blk_zone), GFP_KERNEL | __GFP_ZERO);
	if (!dmzap->internal_zones)
		goto err_free_conv;

	dmzap->meta_zones = kvmalloc_array(dmzap->nr_meta_zones,
				sizeof(struct dmzap_meta_zone),
				GFP_KERNEL | __GFP_ZERO);
	if (!dmzap->meta_zones)
		goto err_free_internal;

	dmzap->rand_zones = kvmalloc_array(dmzap->nr_rand_zones,
				sizeof(struct dmzap_rand_zone),
				GFP_KERNEL | __GFP_ZERO);
	if (!dmzap->rand_zones)
		goto err_free_meta;

	/* Set up conventional zone descriptors */
	for (i = 0; i < dmzap->nr_conv_zones; i++) {
		struct blk_zone *zone = &dmzap->conv_zones[i];

		zone->start = zone->wp = sector;
		zone->len = dev->zone_nr_sectors;
		// zone->capacity = dev->zone_nr_sectors;
		zone->type = BLK_ZONE_TYPE_CONVENTIONAL;
		zone->cond = BLK_ZONE_COND_NOT_WP;

		sector += dev->zone_nr_sectors;
	}

	/* Get set up internal zone descriptors */
	nr_zones = dmzap->nr_internal_zones;
	ret = blkdev_report_zones(dev->bdev, 0, dmzap->internal_zones,
			&nr_zones, GFP_KERNEL);

	if (ret != 0 || nr_zones != (dmzap->nr_internal_zones)) {
		dmz_dev_err(dev, "failed to get internal zone descriptors");
		goto err_free_rand;
	}

	for (i = 0; i < dmzap->nr_meta_zones; i++)
		dmzap->meta_zones[i].zone = &dmzap->internal_zones[i];

	for (j = 0; j < dmzap->nr_rand_zones; j++, i++)
		dmzap->rand_zones[j].zone = &dmzap->internal_zones[i];

	/* TODO: create a prepare_zone function */
	dmzap->rand_wp = dmzap->rand_zones[0].zone->start;

	return 0;

err_free_rand:
	kvfree(dmzap->rand_zones);
err_free_meta:
	kvfree(dmzap->meta_zones);
err_free_internal:
	kvfree(dmzap->internal_zones);
err_free_conv:
	kvfree(dmzap->conv_zones);

	return -ENOMEM;
}

void dmzap_zones_free(struct dmzap_target *dmzap)
{
	kvfree(dmzap->conv_zones);
	kvfree(dmzap->internal_zones);
	kvfree(dmzap->meta_zones);
	kvfree(dmzap->rand_zones);
}


int dmzap_geometry_init(struct dm_target *ti, unsigned int nr_conv_zones)
{
	struct dmzap_target *dmzap = ti->private;
	struct dmz_dev *dev = dmzap->dev;
	unsigned int dev_zones = dev->nr_zones;
	unsigned int user_zones;
	unsigned int op;

	if (nr_conv_zones > 1 )
		op = (2 * nr_conv_zones) / (nr_conv_zones - 1) + 1;
	else
		op = 1;

	dmzap->nr_rand_zones = nr_conv_zones + op;
	dmzap->nr_meta_zones = 2;

	dmzap->nr_internal_zones = dmzap->nr_rand_zones + dmzap->nr_meta_zones;

	if (dmzap->nr_internal_zones > dev_zones) {
		ti->error = "not enough zones on backing device";
		return -EINVAL;
	}

	dmzap->nr_conv_zones = nr_conv_zones;
	dmzap->nr_seq_zones = dev_zones - dmzap->nr_internal_zones;
	dmzap->seq_zones_start = dmzap->nr_internal_zones
				 << dev->zone_nr_sectors_shift;

	dmzap->seq_zones_offset = (dmzap->nr_internal_zones - nr_conv_zones)
				  << dev->zone_nr_sectors_shift;

	user_zones = dmzap->nr_conv_zones + dmzap->nr_seq_zones;
	dmzap->capacity = user_zones << dev->zone_nr_sectors_shift;

	return 0;
}

/*
 * Setup target.
 */
static int dmzap_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dmzap_target *dmzap;
	struct dmz_dev *dev;
	unsigned int nr_conv_zones;
	char dummy;
	int ret;

	/* We reuse macros and other stuff from dm-zoned.
	 * Make sure that dm-zoned does not unexpectedly change
	 * block size.
	 */
	BUILD_BUG_ON(DMZ_BLOCK_SIZE != 4096);

	/* check arguments */
	if (argc != 2) {
		ti->error = "invalid argument count";
		return -EINVAL;
	}

	if (sscanf(argv[1], "%u%c", &nr_conv_zones, &dummy) != 1) {
		ti->error = "Invalid number of conventional zones";
		return -EINVAL;
	}

	/* allocate and initialize the target descriptor */
	dmzap = kzalloc(sizeof(struct dmzap_target), GFP_KERNEL);
	if (!dmzap) {
		ti->error = "unable to allocate the zoned target descriptor";
		return -ENOMEM;
	}
	ti->private = dmzap;

	/* get the target zoned block device */
	ret = dmzap_get_zoned_device(ti, argv[0]);
	if (ret) {
		dmzap->ddev = NULL;
		goto err;
	}

	ret = dmzap_geometry_init(ti, nr_conv_zones);
	if (ret) {
		goto err_dev;
	}

	dev = dmzap->dev;

	ret = dmzap_zones_init(dmzap);
	if (ret) {
		ti->error = "failed to initialize zones";
		goto err_dev;
	}

	ret = dmzap_map_init(dmzap);
	if (ret) {
		ti->error = "failed to initialize mapping table";
		goto err_zones;
	}

	/* set target (no write same support) */
	ti->max_io_len = dev->zone_nr_sectors << 9;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->num_write_zeroes_bios = 1;
	ti->per_io_data_size = sizeof(struct dmzap_bioctx);
	ti->flush_supported = true;
	ti->discards_supported = true;

	ti->len = dmzap->capacity;

	ret = bioset_init(&dmzap->bio_set, DMZAP_MIN_BIOS, 0, 0);
	if (ret) {
		ti->error = "create bio set failed";
		goto err_map;
	}

	dmz_dev_info(dev, "target internal zones: %u meta, %u rand",
			dmzap->nr_meta_zones, dmzap->nr_rand_zones);

	dmz_dev_info(dev, "target device: user zones: %u conventional, %u sequential",
			dmzap->nr_conv_zones, dmzap->nr_seq_zones);

	dmz_dev_info(dev, "target device: %llu 512-byte logical sectors (%llu blocks)",
		     (unsigned long long)ti->len,
		     (unsigned long long)dmz_sect2blk(ti->len));


	dmz_dev_info(dev, "random area write pointer: %llu",
			(unsigned long long)dmzap->rand_wp);
	return 0;

err_map:
	dmzap_map_free(dmzap);
err_zones:
	dmzap_zones_free(dmzap);
err_dev:
	dmzap_put_device(ti);
err:
	kfree(dmzap);

	return ret;
}

/*
 * Cleanup target.
 */
static void dmzap_dtr(struct dm_target *ti)
{
	struct dmzap_target *dmzap = ti->private;

	bioset_exit(&dmzap->bio_set);
	dmzap_put_device(ti);
	dmzap_zones_free(dmzap);
	dmzap_map_free(dmzap);
	kfree(dmzap);
}

/*
 * Pass on ioctl to the backend device.
 */
static int dmzap_prepare_ioctl(struct dm_target *ti, struct block_device **bdev)
{
	struct dmzap_target *dmzap = ti->private;

	/* TODO: do we really want to just pipe things through?*/
	*bdev = dmzap->dev->bdev;

	return 0;
}

/*
 * Setup target request queue limits.
 */
static void dmzap_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct dmzap_target *dmzap = ti->private;
	unsigned int zone_sectors = dmzap->dev->zone_nr_sectors;

	limits->logical_block_size = DMZ_BLOCK_SIZE;
	limits->physical_block_size = DMZ_BLOCK_SIZE;

	blk_limits_io_min(limits, DMZ_BLOCK_SIZE);
	blk_limits_io_opt(limits, DMZ_BLOCK_SIZE);

	limits->discard_alignment = DMZ_BLOCK_SIZE;
	limits->discard_granularity = DMZ_BLOCK_SIZE;
	limits->max_discard_sectors = zone_sectors;
	limits->max_hw_discard_sectors = zone_sectors;
	limits->max_write_zeroes_sectors = zone_sectors;

	/* FS hint to try to align to the device zone size */
	limits->chunk_sectors = zone_sectors;
	limits->max_sectors = zone_sectors;

	/* We are exposing a host-managed zoned block device */
	limits->zoned = BLK_ZONED_HM;
}

/*
 * Stop background work on suspend.
 */
static void dmzap_suspend(struct dm_target *ti)
{
	/* struct dmzap_target *dmz = ti->private;
	 * TODO: Implement suspend
	 * */
}

/*
 * Resume background work
 */
static void dmzap_resume(struct dm_target *ti)
{
	/* struct dmzap_target *dmz = ti->private;
	 * TODO: implement resume */
}

static int dmzap_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct dmzap_target *dmzap = ti->private;
	struct dmz_dev *dev = dmzap->dev;
	sector_t capacity = dev->capacity;

	/*  TODO: validate this */
	return fn(ti, dmzap->ddev, 0, capacity, data);
}

static struct target_type dmzap_type = {
	.name		 = "zap",
	.version	 = {1, 0, 0},
	.features	 = DM_TARGET_ZONED_HM,
	.module		 = THIS_MODULE,
	.report_zones	 = dmzap_report_zones,
	.ctr		 = dmzap_ctr,
	.dtr		 = dmzap_dtr,
	.map		 = dmzap_map,
	.io_hints	 = dmzap_io_hints,
	.prepare_ioctl	 = dmzap_prepare_ioctl,
	.postsuspend	 = dmzap_suspend,
	.resume		 = dmzap_resume,
	.iterate_devices = dmzap_iterate_devices,
};

static int __init dmzap_init(void)
{
	return dm_register_target(&dmzap_type);
}

static void __exit dmzap_exit(void)
{
	dm_unregister_target(&dmzap_type);
}

module_init(dmzap_init);
module_exit(dmzap_exit);

MODULE_DESCRIPTION(DM_NAME " target");
MODULE_AUTHOR("Hans Holmberg <hans.holmberg@wdc.com>");
MODULE_LICENSE("GPL");
