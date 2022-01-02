// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 */

#include <linux/types.h>
#include <linux/blkdev.h>
#include "dm-zoned.h"
#include <linux/crc32.h>
#include <linux/list.h>
#include <linux/sort.h>
#include <linux/rbtree.h>


#define	DM_MSG_PREFIX		"zap"

#define DM_ZAP_MAGIC	0x72927048	/* "zap0" */
#define DM_ZAP_VERSION	1

#define DMZAP_UNMAPPED (~((sector_t) 0))
#define DMZAP_INVALID (DMZAP_UNMAPPED - 1)

/*
 * Flush intervals (seconds).
 */
#define DMZAP_FLUSH_PERIOD	(10 * HZ)

/*
 * Delta period, in which a zone does not have to go into class 1  (seconds).
 */
#define DMZAP_CLASS_0_DELTA_PERIOD	(2 * HZ)

#define DEF_HEAP_SIZE 1000

#define HEAP_SIZE_SHRINK (DEF_HEAP_SIZE/2 - DEF_HEAP_SIZE/10)

/*
 * dm-zap internal zone types
 */
enum {
	DMZAP_META,
	DMZAP_RAND,
	DMZAP_OP,
};

/*
 * dm-zap internal zone state
 */
enum {
	DMZAP_CLEAN,
	DMZAP_OPENED,
};

/*
 * Reclaim state flags.
 */
enum {
	DMZAP_RECLAIM_KCOPY,
};

/*
 * dm-zap managed zones descriptor.
 */
struct dmzap_zone {
	struct blk_zone		*zone; /* Backing zone = dmzap->backing_seq_zones[seq] */
	unsigned int		type;	/* DMZAP_ META/RAND/OP */
	u32			seq;	/* Sequence id / id of the zone*/ //TODO maybe increment as header_seq_nr
	unsigned int		state; /* DMZAP_ CLEAN/OPENED */
	int		nr_invalid_blocks; /* Invalid block counter */

	/* Last zone modification time */
	unsigned long zone_age;
	/* Time when this zone should shift to reclaim class 0 */
	/* If already in class 0 the shift_time is in the past*/
	unsigned long shift_time;

	/* CostBenefit value */
	long long cb;

	/* FeGC */
	unsigned int fegc_heap_pos;
	unsigned long long cwa;
	unsigned long long cwa_time;

	/* FaGC+ */
	unsigned long long cps;

	struct list_head	num_invalid_blocks_link;
	//struct list_head	cb_l

	struct list_head	link;
	struct rb_node		node;
	int reclaim_class;
	struct mutex reclaim_class_lock;

	struct mutex lock;
};

struct dmzap_fegc_heap{
	unsigned int max_size;
	unsigned int size;
	struct dmzap_zone **data;
	struct mutex lock;
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
	struct blk_zone		*zone; /* Backing zone */
};

struct dmzap_rand_zone {
	struct blk_zone		*zone; /* Backing zone */
};

struct dmzap_map {
	sector_t		*l2d;	/* Logical to device mapping */
	sector_t		*d2l;	/* Device to logical mapping */
	sector_t l2d_sz; /* Size of the l2d mapping table */
	sector_t nr_total_blocks; /* Size of all random blocks */
	int *invalid_device_block; /* invalid_device_block Flag if a device block is marked invalid (refering to l2d mapping) */
	struct mutex map_lock;
};

/*
 * Chunk work descriptor.
*/
struct dmzap_chunk_work {
	struct work_struct	work;
	refcount_t		refcount;
	struct dmzap_target	*target;
	unsigned int		chunk;
	struct bio_list		bio_list;
};

enum {
	DMZAP_WR_OUTSTANDING,
};

enum {
	DMZAP_GREEDY,
	DMZAP_CB,
	DMZAP_FAST_CB,
	DMZAP_APPROX_CB,
	DMZAP_CONST_GREEDY,
	DMZAP_CONST_CB,
	DMZAP_FEGC,
	DMZAP_FAGCPLUS,
	DMZAP_VICTIM_POLICY_MAX

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

	unsigned int overprovisioning_rate;
	unsigned int class_0_cap;
	unsigned int class_0_optimal;
	unsigned int victim_selection_method; /* 0 = greedy, 1 = cb, 2 = fast cb */

	/* Capacities */
	unsigned int		nr_internal_zones;
	unsigned int 		nr_op_zones;
	unsigned int		nr_meta_zones;
	unsigned int		nr_user_exposed_zones; /* internal - op - meta*/

	/* Sequential zones */
	struct dmzap_zone *dmzap_zones;
	/* Backing blockdevice zones*/
	struct blk_zone		*internal_zones;
	/* write pointer that indicates the active zone */
	u32 dmzap_zone_wp;

	//TODO pointers to list head
	/* Pointer to user zones */
	struct dmzap_zone **user_zones;
	/* Pointer to overprovisioning zones */
	struct dmzap_zone **op_zones;
	/* Pointer to meta zones */
	struct dmzap_zone **meta_zones;

	spinlock_t	meta_blk_lock;

	/* Mapping information */
	struct dmzap_map	map;
	atomic_t header_seq_nr;

	/* Write serialization for conventional zones */
	unsigned long		write_bitmap;   /* Conventional zone write state bitmap */

	/* For reclaim */
	sector_t		capacity;
	sector_t		dev_capacity;
	struct dmzap_reclaim	*reclaim;
	struct mutex		reclaim_lock;
	unsigned int reclaim_limit;

	struct list_head*	num_invalid_blocks_lists; // Index is the number of invalid blocks

	/* For fast CB reclaim */
	struct list_head	reclaim_class_0;
	struct rb_root reclaim_class_1_rbtree; //todo destruct

	unsigned int		nr_reclaim_class_0;
	unsigned int		nr_reclaim_class_1;
	long long threshold_cb;

	struct list_head	q_list;
	unsigned int		q_cap;
	unsigned int		q_length;


	/* For chunk work */
	struct radix_tree_root	chunk_rxtree;
	struct workqueue_struct *chunk_wq;
	struct mutex		chunk_lock;

	/* For flush */
	spinlock_t		flush_lock;
	struct bio_list		flush_list;
	struct delayed_work	flush_work;
	struct workqueue_struct *flush_wq;

	//TODO do we need those numbers?
	unsigned int nr_clean_zones;
	unsigned int nr_opened_zones;

	/* Last wa print time time */
	unsigned long		wa_print_time;

	unsigned int show_debug_msg; //TODO remove
	u64 nr_user_written_sec;
	u64 nr_gc_written_sec;
	int debug_int;

	u64 gc_time;
	u64 gc_count;

	spinlock_t debug_lock;
	unsigned long		flags; //TODO maybe not needed. -> used for wait_on_bit_io to wait for reclaim before writing.

	struct dmzap_fegc_heap **fegc_heaps;

	unsigned int *fagc_cps;
	u64 current_write_num;
	struct dmzap_fegc_heap fagc_heap;
	struct mutex *user_zone_locks;

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

/* dm-zap-target.c */
bool dmzap_bdev_is_dying(struct dmz_dev *dmz_dev);

/* dm-zap-map.c */
int dmzap_map_init(struct dmzap_target *dmzap);
void dmzap_map_free(struct dmzap_target *dmzap);
int dmzap_map_update(struct dmzap_target *dmzap, sector_t user, sector_t backing,
	      sector_t len);
int dmzap_map_update_if_eq(struct dmzap_target *dmzap, sector_t user, sector_t backing,
	      sector_t len, sector_t *orig_secs);
int dmzap_map_lookup(struct dmzap_target *dmzap, sector_t user, sector_t *backing,
	      sector_t len);
int dmzap_get_invalid_flag_ret_user_block(struct dmzap_target *dmzap,
  			struct dmzap_zone *zone, sector_t chunk_block, sector_t *ret_user_block);
int dmzap_get_invalid_flag(struct dmzap_target *dmzap,
  			struct dmzap_zone *zone, sector_t chunk_block);
void dmzap_unmap_zone_entries(struct dmzap_target *dmzap,
  			struct dmzap_zone *zone);
int dmzap_remap_copy(struct dmzap_target *dmzap,
	sector_t read_backing_block, sector_t write_backing_block, sector_t len);


sector_t dmzap_get_seq_wp(struct dmzap_target *dmzap);
void dmzap_update_seq_wp(struct dmzap_target *dmzap, sector_t bio_sectors);
inline void print_mapping(struct dmzap_target *dmzap);
int dmzap_handle_discard(struct dmzap_target *dmzap, struct bio *bio);
int dmzap_map_seq(struct dmzap_target *dmzap, struct bio *bio);
int dmzap_conv_write(struct dmzap_target *dmzap, struct bio *bio);
int dmzap_conv_read(struct dmzap_target *dmzap, struct bio *bio);


/*
 * Reclaim context
 */
struct dmzap_reclaim
{
	struct dmzap_target *dmzap;
	unsigned long nr_free_user_zones;
	unsigned int p_free_user_zones;

	unsigned long nr_free_zones;
	unsigned int p_free_zones;

	/* CostBenefit values */
	long long *cb;

	/* Last target access time */
	unsigned long		atime;

	struct delayed_work	work;
	struct workqueue_struct *wq;

	struct dm_kcopyd_client	*kc;
	struct dm_kcopyd_throttle kc_throttle;
	int			kc_err;

	unsigned long		flags;

};

#define DMZAP_CB_SCALE_FACTOR 1000
#define DMZAP_START_THRESHOLD_CB 15000

/*
 * Number of seconds of target BIO inactivity to consider the target idle.
 */
#define DMZAP_IDLE_PERIOD		(10UL * HZ)

/*
 * Number of seconds wa should be trace_printk'ed
 */
#define DMZAP_WA_PERIOD		(5UL * HZ)

#define dmzap_bio_chunk(dev, bio)	((bio)->bi_iter.bi_sector >> \
				 ilog2((dev)->zone_nr_sectors))
#define dmzap_chunk_block(dev, b)	((b) & (dmz_sect2blk((dev)->zone_nr_sectors) - 1))

//TODO (IMPORTANT) use as much const parameters as possible
/*
 * Functions defined in dm-zoned-reclaim.c
 */
int dmzap_ctr_reclaim(struct dmzap_target *dmzap);
void dmzap_dtr_reclaim(struct dmzap_target *dmzap);
void dmzap_schedule_reclaim(struct dmzap_target *dmzap);
void dmzap_reclaim_bio_acc(struct dmzap_target *dmzap);
void dmzap_resume_reclaim(struct dmzap_target *dmzap);
void dmzap_suspend_reclaim(struct dmzap_target *dmzap);
void dmzap_calc_p_free_zone(struct dmzap_target *target);

int dmzap_invalidate_blocks(struct dmzap_target *dmzap, sector_t backing_block,
	unsigned int nr_blocks);

long long dmzap_calc_cb_value(struct dmzap_target *dmzap,
  const struct dmzap_zone *zone, unsigned long currentTime);

bool dmzap_check_bdev(struct dmz_dev *dmz_dev);
inline void dmzap_bio_endio(struct bio *bio, blk_status_t status);

void dmzap_assign_zone_to_reclaim_class(struct dmzap_target *dmzap,
  struct dmzap_zone *zone);

//do not have it public
int dmzap_free_victim (struct dmzap_target *dmzap, struct dmzap_zone *victim);

/* dm-zap-heap.c */
void dmzap_heap_insert(struct dmzap_fegc_heap *heap, struct dmzap_zone *zone);
struct dmzap_zone* dmzap_heap_delete(struct dmzap_fegc_heap *heap, struct dmzap_zone *pos);
void dmzap_fegc_heap_init(struct dmzap_fegc_heap *heap);
void assert_heap_ok(struct dmzap_target *dmzap, int num);
extern struct dmzap_target* dmzap_ptr;
bool heap_is_ok(struct dmzap_target *dmzap, int heap_num);
void heap_print(struct dmzap_target *dmzap, int heap_num);
extern void dmzap_heap_update(struct dmzap_fegc_heap *heap, unsigned int pos);
extern int dmzap_heap_increase_size(void *arg);
extern void dmzap_heap_destroy(struct dmzap_fegc_heap *heap);
extern unsigned long long updated_cwa(struct dmzap_zone *zone, u64 jiff);
extern void assert_heap_is_ok(struct dmzap_target *dmzap, struct dmzap_fegc_heap *heap);
extern struct mutex heap_increase_lock;
