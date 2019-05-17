// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 */

#include "dm-zap.h"

int dmzap_map_init(struct dmzap_target *dmzap)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t l2d_sz, i;

	l2d_sz = dmzap->capacity >> 3;

	map->l2d = kvmalloc_array(l2d_sz, sizeof(sector_t), GFP_KERNEL);
	if (!map->l2d)
		return -ENOMEM;

	for (i = 0; i < l2d_sz; i++)
		map->l2d[i] = DMZAP_UNMAPPED;

	return 0;
}

void dmzap_map_free(struct dmzap_target *dmzap)
{
	struct dmzap_map *map = &dmzap->map;

	kvfree(map->l2d);
}

/* Map len blocks */
int dmzap_map_update(struct dmzap_target *dmzap, sector_t user, sector_t backing,
	      sector_t len)
{
	struct dmzap_map *map = &dmzap->map;

	/* Out of bounds? */
	if ((user + len) > (dmzap->capacity >> 3))
		BUG();

	dmz_dev_debug(dmzap->dev, "mapping %d user block(s) from %d to backing block: %d",
			(int)len, (int)user, (int)backing);

	/* TODO: invalidate old mapping */

	while (len--)
		map->l2d[user++] = backing++;

	return 0;
}

/* Map len blocks if still mapped to orig_secs[] */
int dmzap_map_update_if_eq(struct dmzap_target *dmzap, sector_t user, sector_t backing,
	      sector_t len, sector_t *orig_secs)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t m = 0;

	/* Out of bounds? */
	if ((user + len) > (dmzap->capacity >> 3))
		return -1;

	/* TODO: invalidate old mapping */

	while (len--) {
		if (orig_secs[m++] == map->l2d[user])
			map->l2d[user] = backing;
		user++;
		backing++;
	}

	return 0;
}


/* Look up mapped blocks, returns:
 * number of contigous sectors (<= len)
 */
int dmzap_map_lookup(struct dmzap_target *dmzap, sector_t user, sector_t *backing,
	      sector_t len)
{
	struct dmzap_map *map = &dmzap->map;
	sector_t m = user;
	sector_t left = len - 1;

	/* Out of bounds? */
	if ((user + len) > dmz_sect2blk(dmzap->capacity))
		BUG();

	*backing = map->l2d[user];

	if (map->l2d[user] == DMZAP_UNMAPPED) {
		while (left && map->l2d[m] == DMZAP_UNMAPPED) {
			m++;
			left--;
		}
	} else {
		while (left && ((map->l2d[m] + 1) == map->l2d[m+1])) {
			m++;
			left--;
		}
	}

	dmz_dev_debug(dmzap->dev, "looked up %d user block(s) from %d to backing block: %d",
			(int)(len - left), (int)user, (int)*backing);

	return len - left;
}
