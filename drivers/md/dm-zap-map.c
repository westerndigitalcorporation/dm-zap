// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 */

#include "dm-zap.h"

// All mapping methods must be called with the map_lock hold.

/*
 * Initialize mapping
 */
int dmzap_map_init(struct dmzap_target *dmzap)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t l2d_sz, i;

	l2d_sz = dmzap->dev_capacity >> 3;

	map->l2d_sz = l2d_sz;

	map->l2d = kvmalloc_array(l2d_sz, sizeof(sector_t), GFP_KERNEL);
	if (!map->l2d)
		return -ENOMEM;

	map->d2l = kvmalloc_array(l2d_sz, sizeof(sector_t), GFP_KERNEL);
	if (!map->d2l){
		kvfree(map->l2d);
		return -ENOMEM;
	}

	map->nr_total_blocks = dmz_sect2blk(dmzap->dev->zone_nr_sectors) * dmzap->dev->nr_zones;

	map->invalid_device_block = kvmalloc_array(map->nr_total_blocks, sizeof(int), GFP_KERNEL | __GFP_ZERO);
	if (!map->invalid_device_block){
		kvfree(map->l2d);
		kvfree(map->d2l);
		return -ENOMEM;
	}

	for (i = 0; i < l2d_sz; i++){
		map->l2d[i] = DMZAP_UNMAPPED;
		map->d2l[i] = DMZAP_UNMAPPED;
	}

	mutex_init(&map->map_lock);
	return 0;
}

void dmzap_map_free(struct dmzap_target *dmzap)
{
	struct dmzap_map *map = &dmzap->map;
	mutex_destroy(&map->map_lock);
	kvfree(map->l2d);
	kvfree(map->d2l);
	kvfree(map->invalid_device_block);
}

/* Map len blocks */
int dmzap_map_update(struct dmzap_target *dmzap,
	sector_t user, sector_t backing, sector_t len)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t backing_block;

	/* Out of bounds? */
	if ((user + len) > (dmzap->dev_capacity >> 3)){
		BUG();
		return -1;
	}
	if(dmzap->show_debug_msg){
		dmz_dev_debug(dmzap->dev, "mapping %d user block(s) from %d to backing block: %d",
				(int)len, (int)user, (int)backing);
	}

	while (len--) {
		backing_block = map->l2d[user];

		/* Invalidate old mapping */
		if(backing_block != DMZAP_UNMAPPED){
			dmzap_invalidate_blocks(dmzap, backing_block, 1);
		}
		/* New mapping */
		map->l2d[user] = backing;
		map->d2l[backing] = user;
		backing++;
		user++;
	}

	return 0;
}


/* Map len blocks */
int dmzap_remap_copy(struct dmzap_target *dmzap,
	sector_t read_backing_block, sector_t write_backing_block, sector_t len)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t user = 0;

	//TODO /* Out of bounds? */

	while (len--) {

		/* Get user block of old mapping */
		user = map->d2l[read_backing_block];

		/* Invalidate old mapping */
		//dmzap_invalidate_blocks(dmzap, read_backing_block, 1); //TODO (Check if we need to invalidate since the victim gets validated anyway)

		/* New mapping */
		map->l2d[user] = write_backing_block;
		map->d2l[write_backing_block] = user;
		write_backing_block++;
		read_backing_block++;
	}

	return 0;
}




/* Map len blocks if still mapped to orig_secs[] */
int dmzap_map_update_if_eq(struct dmzap_target *dmzap,
	sector_t user, sector_t backing, sector_t len, sector_t *orig_secs)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t backing_block;
	sector_t m = 0;

	/* Out of bounds? */
	if ((user + len) > (dmzap->capacity >> 3)){
		BUG();
		return -1;
	}

	while (len--) {
		backing_block = map->l2d[user];

		if (orig_secs[m++] == backing_block){
			/* Invalidate old mapping */
			if(backing_block != DMZAP_UNMAPPED){
				dmzap_invalidate_blocks(dmzap, backing_block, 1);
			}
			/* New mapping */
			map->l2d[user] = backing;
			map->d2l[backing] = user;
		}
		user++;
		backing++;
	}

	return 0;
}


/*
 * Look up mapped blocks, returns:
 * number of contigous sectors (<= len)
 */
int dmzap_map_lookup(struct dmzap_target *dmzap,
				sector_t user, sector_t *backing, sector_t len)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t m = user;
	sector_t left = len - 1;

	/* Out of bounds? */
	if ((user + len) > dmz_sect2blk(dmzap->capacity)) {
		BUG();
		return -1;
	}

	*backing = map->l2d[user];

	if (map->l2d[user] == DMZAP_UNMAPPED) {
		*backing = DMZAP_UNMAPPED;
		while (left && (map->l2d[m] == DMZAP_UNMAPPED)) {
			m++;
			left--;
		}
	} else if(map->l2d[user] >= 0 && map->invalid_device_block[map->l2d[user]]){
		*backing = DMZAP_INVALID;
		while (left && (map->l2d[m] >= 0
			&& map->invalid_device_block[map->l2d[m]])) {
			m++;
			left--;
		}
	} else {
		while (left && ((map->l2d[m] + 1) == map->l2d[m+1])
			&& !map->invalid_device_block[map->l2d[m]] && !map->invalid_device_block[map->l2d[m+1]] ) {
			m++;
			left--;
		}
	}

	if(dmzap->show_debug_msg){
		dmz_dev_debug(dmzap->dev, "looked up %d user block(s) from %d to backing block: %d",
				(int)(len - left), (int)user, (int)*backing);
	}

	return len - left;
}

/*
 * Get the invalid flag from the given chunk_block of the given zone.
 * If the block is still valid the ret_user_block is set with the locical block address.
 */
int dmzap_get_invalid_flag_ret_user_block(struct dmzap_target *dmzap,
  struct dmzap_zone *zone, sector_t chunk_block, sector_t *ret_user_block)
{
  sector_t i = 0;
  int ret = -1;
  sector_t block_nr = dmz_sect2blk(zone->zone->start) + chunk_block;

  *ret_user_block = -1;

	if(block_nr >= dmzap->map.nr_total_blocks || block_nr < 0){
		dmz_dev_err(dmzap->dev, "Trying to access invlaid flag out of bounds.\n");
		return -EFAULT;
	}

	if ((ret = dmzap->map.invalid_device_block[block_nr]) == 0) {
    for (i = 0; i < dmzap->map.l2d_sz; i++) {
      if (dmzap->map.l2d[i] == block_nr) {
        *ret_user_block = i;
				goto out;
      }
    }
  }

out:
  return ret;
}

/*
 * Get the invalid flag from the given chunk_block of the given zone.
 */
int dmzap_get_invalid_flag(struct dmzap_target *dmzap,
  struct dmzap_zone *zone, sector_t chunk_block)
{

  sector_t block_nr = dmz_sect2blk(zone->zone->start) + chunk_block;

	if(block_nr >= dmzap->map.nr_total_blocks || block_nr < 0){
		dmz_dev_err(dmzap->dev, "Trying to access invlaid flag out of bounds.\n");
		return -EFAULT;
	}

  return dmzap->map.invalid_device_block[block_nr];
}

/*
 * Unmap all blocks of a given zone
 */
void dmzap_unmap_zone_entries(struct dmzap_target *dmzap,
  struct dmzap_zone *zone)
{
	sector_t start_block = dmz_sect2blk(zone->zone->start);
	sector_t current_entry;
	int i = 0;

	for(i = 0; i < dmzap->map.l2d_sz; i++){
    current_entry = dmzap->map.l2d[i];
    if( current_entry >= start_block
      && current_entry < (start_block + dmz_sect2blk(dmzap->dev->zone_nr_sectors)) ){
			dmzap->map.d2l[current_entry] = DMZAP_UNMAPPED; //TODO is getting much faster with that datastructure now
      dmzap->map.l2d[i] = DMZAP_UNMAPPED;
    }
  }
}
