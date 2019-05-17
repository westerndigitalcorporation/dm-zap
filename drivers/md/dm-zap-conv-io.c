// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 */

#include "dm-zap.h"

static inline void dmzap_bio_end_wr(struct bio *bio, blk_status_t status)
{
	struct dmzap_bioctx *bioctx;
	struct dmzap_target *dmzap;

	bioctx = dm_per_bio_data(bio, sizeof(struct dmzap_bioctx));
	dmzap = bioctx->target;

	if (bio->bi_status != BLK_STS_OK) {
		/* TODO: stop writing to zone
		 *  (or writing altogether) on
		 *  failed writes
		 */
		dmz_dev_err(dmzap->dev, "write failed!");
	} else {
		int ret;

		ret = dmzap_map_update(dmzap,
			dmz_sect2blk(bioctx->user_sec),
			dmz_sect2blk(dmzap->rand_wp),
			dmz_bio_blocks(bio));

		dmzap->rand_wp += bio_sectors(bio);

		if(ret)
			dmz_dev_err(dmzap->dev, "endio mapping failed!");
	}

	clear_bit_unlock(DMZAP_WR_OUTSTANDING, &dmzap->write_bitmap);
}

static inline void dmzap_put_bio(struct bio *bio)
{
	struct dmzap_bioctx *bioctx;

	bioctx = dm_per_bio_data(bio, sizeof(struct dmzap_bioctx));

	if (refcount_dec_and_test(&bioctx->ref))
		bio_endio(bio);
}

static inline void dmzap_get_bio(struct bio *bio)
{
	struct dmzap_bioctx *bioctx;

	bioctx = dm_per_bio_data(bio, sizeof(struct dmzap_bioctx));
	refcount_inc(&bioctx->ref);
}

/*
 * Completion callback for an internally cloned target BIO. This terminates the
 * target BIO when there are no more references to its context.
 */
static void dmzap_clone_endio(struct bio *clone)
{
	struct dmzap_bioctx *bioctx = clone->bi_private;
	blk_status_t status = clone->bi_status;
	struct bio *bio = bioctx->bio;

	if (status != BLK_STS_OK && bio->bi_status == BLK_STS_OK)
		bio->bi_status = status;

	if (bio_data_dir(bio) == WRITE)
		dmzap_bio_end_wr(bioctx->bio, status);

	dmzap_put_bio(bio);
	bio_put(clone);
}

/*
 * Issue a clone of a target BIO. The clone may only partially process the
 * original target BIO.
 */
static int dmzap_submit_bio(struct dmzap_target *dmzap, sector_t sector,
			  struct bio *bio)
{
	struct dmzap_bioctx *bioctx;
	struct bio *clone;

	bioctx = dm_per_bio_data(bio, sizeof(struct dmzap_bioctx));
	clone = bio_clone_fast(bio, GFP_NOIO, &dmzap->bio_set);
	if (!clone)
		return -ENOMEM;

	bio_set_dev(clone, dmzap->dev->bdev);

	clone->bi_iter.bi_sector = sector;
	clone->bi_end_io = dmzap_clone_endio;
	clone->bi_private = bioctx;
	generic_make_request(clone);

	return 0;
}

static inline void dmzap_init_bioctx(struct dmzap_target *dmzap,
				     struct bio *bio)
{
	struct dmzap_bioctx *bioctx;

	bioctx = dm_per_bio_data(bio, sizeof(struct dmzap_bioctx));
	bioctx->target = dmzap;
	bioctx->user_sec = bio->bi_iter.bi_sector;
	bioctx->bio = bio;
	refcount_set(&bioctx->ref, 1);
}

static int dmzap_conv_read(struct dmzap_target *dmzap, struct bio *bio)
{
	sector_t user = bio->bi_iter.bi_sector;
	unsigned int size;
	sector_t backing;
	int mapped;
	sector_t left = dmz_bio_blocks(bio);
	int ret;

	dmzap_init_bioctx(dmzap, bio);

	while (left) {
		mapped = dmzap_map_lookup(dmzap,
				dmz_sect2blk(user),
				&backing, left);

		size = mapped << DMZ_BLOCK_SHIFT;

		if (backing == DMZAP_UNMAPPED) {

			swap(bio->bi_iter.bi_size, size);
			zero_fill_bio(bio);
			swap(bio->bi_iter.bi_size, size);

		} else {
			dmzap_get_bio(bio);

			ret = dmzap_submit_bio(dmzap, dmz_blk2sect(backing), bio);
			if (ret)
				return DM_MAPIO_KILL;
		}

		bio_advance(bio, size);
		left -= mapped;
	}

	dmzap_put_bio(bio);

	return DM_MAPIO_SUBMITTED;
}

static int dmzap_conv_write(struct dmzap_target *dmzap, struct bio *bio)
{
	int ret;

	/* We can only have one outstanding write at a time */
	while(test_and_set_bit_lock(DMZAP_WR_OUTSTANDING,
				&dmzap->write_bitmap))
		io_schedule();

	dmzap_init_bioctx(dmzap, bio);

	ret = dmzap_submit_bio(dmzap, dmzap->rand_wp, bio);
	if (ret) {
		/* Out of memory, try again later */
		clear_bit_unlock(DMZAP_WR_OUTSTANDING, &dmzap->write_bitmap);
		return DM_MAPIO_REQUEUE;
	}

	return DM_MAPIO_SUBMITTED;
}

int dmzap_map_conv(struct dmzap_target *dmzap, struct bio *bio)
{
	int ret;

	if (!bio_sectors(bio)) {
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		ret = dmzap_conv_read(dmzap, bio);
		break;
	case REQ_OP_WRITE:
		ret = dmzap_conv_write(dmzap, bio);
		break;
	default:
		dmz_dev_err(dmzap->dev, "Ignoring unsupported BIO operation 0x%x",
			    bio_op(bio));
		bio_endio(bio);
		ret = DM_MAPIO_SUBMITTED;
	}

	return ret;
}

