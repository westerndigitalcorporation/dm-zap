#include "dm-zap.h"

/*
 * dm_kcopyd_copy end notification.
 */
static void dmzap_reclaim_kcopy_end(int read_err, unsigned long write_err,
				  void *context)
{
	struct dmzap_reclaim *reclaim = context;

	if (read_err || write_err)
		reclaim->kc_err = -EIO;
	else
		reclaim->kc_err = 0;

	clear_bit_unlock(DMZAP_RECLAIM_KCOPY, &reclaim->flags);
	smp_mb__after_atomic();
	wake_up_bit(&reclaim->flags, DMZAP_RECLAIM_KCOPY);
}

/*
 *  Convert backing block number to corresponding zone id
 */
unsigned int dmzap_block2zone_id(struct dmzap_target *dmzap, sector_t block)
{
  return ((unsigned int) (block / dmz_sect2blk(dmzap->dev->zone_nr_sectors)));
}

void dmzap_calc_p_free_zone(struct dmzap_target *dmzap)
{
	struct dmzap_reclaim *reclaim = dmzap->reclaim;
	int tmp = 0;
	reclaim->p_free_zones =
		reclaim->nr_free_zones * 100 / dmzap->nr_internal_zones;


	tmp = reclaim->nr_free_zones - dmzap->nr_op_zones - dmzap->nr_meta_zones;
	//TODO the line above is not correct. Use lists to keep track of which zone is a user zone
	if (tmp < 0) {
		reclaim->nr_free_user_zones = 0;
	} else {
		reclaim->nr_free_user_zones = tmp;
	}

	reclaim->p_free_user_zones =
		reclaim->nr_free_user_zones * 100 / dmzap->nr_user_exposed_zones;
}

/*
 * Calculate the time when zone has to be moved to reclaim_class_0
 */
static void dmzap_calc_shift_time(struct dmzap_target *dmzap,
  struct dmzap_zone *zone, long long cb, unsigned long currentTime)
{
  int scale_factor = DMZAP_CB_SCALE_FACTOR;
  int nr_invalid_blocks = zone->nr_invalid_blocks;
  int nr_valid_blocks = dmz_sect2blk(dmzap->dev->zone_nr_sectors) - nr_invalid_blocks; //TODO ZNS capacity: int nr_valid_blocks = dmz_sect2blk(victim->zone->capacity) - nr_invalid_blocks;

  if(cb >= dmzap->threshold_cb){
    zone->shift_time = currentTime;
  }else{
		if (nr_invalid_blocks == 0){
			zone->shift_time = (dmzap->threshold_cb * 2 * nr_valid_blocks) /
				(scale_factor) + zone->zone_age;
		} else {
			zone->shift_time = (dmzap->threshold_cb * 2 * nr_valid_blocks) /
				(nr_invalid_blocks * scale_factor) + zone->zone_age;
		}
  }

  if(dmzap->show_debug_msg){
    dmz_dev_debug(dmzap->dev, "Zone %d shift time: %ld.\n",
          zone->seq, zone->shift_time);
    dmz_dev_debug(dmzap->dev, "threshold_cb %lld, nr_invalid_blocks %d, \
          nr_valid_blocks %d, zone_age %ld, currentTime %ld\n",
          dmzap->threshold_cb, nr_invalid_blocks,
          nr_valid_blocks, zone->zone_age, currentTime);
  }
}

/*
 * Comparator for 64 bit integer
 */
static int compare(const void *lhs, const void *rhs) {
    int lhs_integer = *(const long long *)(lhs);
    int rhs_integer = *(const long long *)(rhs);

    if (lhs_integer < rhs_integer) return -1;
    if (lhs_integer > rhs_integer) return 1;
    return 0;
}

/*
 * Insert a zone in the rbtree for class 1.
 */
static void dmzap_rb_insert_zone_class_1(struct dmzap_target *dmzap,
							struct dmzap_zone *zone)
{
	struct rb_root *root = &dmzap->reclaim_class_1_rbtree;
	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct dmzap_zone *b;
	//trace_printk("Assign zone %d to class 1.\n", zone->seq);

	if(dmzap->nr_reclaim_class_1 < 0 ||
      dmzap->nr_reclaim_class_1 >= dmzap->nr_internal_zones){
    dmz_dev_err(dmzap->dev, "Reclaim class 1 is out of bounds: %d.\n", dmzap->nr_reclaim_class_1);
    //BUG();
    return;
  }

	/* Figure out where to put the new node */
	while (*new) {
		b = container_of(*new, struct dmzap_zone, node);
		parent = *new;
		new = (b->shift_time < zone->shift_time) ? &((*new)->rb_left) : &((*new)->rb_right);
	}

	/* Add new node and rebalance tree */
	rb_link_node(&zone->node, parent, new);
	rb_insert_color(&zone->node, root);

	dmzap->nr_reclaim_class_1++;
	zone->reclaim_class = 1;
}

/*
 * Remove zone from the class 0 list
 */
static void dmzap_unassign_zone_from_class_1(struct dmzap_target *dmzap,
  struct dmzap_zone *zone)
{
	//trace_printk("Unassign zone %d from class 1.\n", zone->seq);

	if(dmzap->nr_reclaim_class_1 <= 0){
    dmz_dev_err(dmzap->dev, "Reclaim class 1 is out of bounds: %d.\n", dmzap->nr_reclaim_class_1);
    //BUG();
    return;
  }


	rb_erase(&zone->node, &dmzap->reclaim_class_1_rbtree);
	dmzap->nr_reclaim_class_1--;
	zone->reclaim_class = -1;
}

/*
 * Put zone in the class 0 list
 */
static void dmzap_assign_zone_to_class_0(struct dmzap_target *dmzap,
  struct dmzap_zone *zone)
{
	//trace_printk("Assign zone %d to class 0.\n", zone->seq);

  if(dmzap->nr_reclaim_class_0 < 0 ||
      dmzap->nr_reclaim_class_0 >= dmzap->nr_internal_zones){
    dmz_dev_err(dmzap->dev, "Reclaim class 0 is out of bounds: %d.\n", dmzap->nr_reclaim_class_0);
    //BUG();
    return;
  }


  list_add_tail(&zone->link, &dmzap->reclaim_class_0);
  dmzap->nr_reclaim_class_0++;
	zone->reclaim_class = 0;
	if(dmzap->show_debug_msg)
  	dmz_dev_debug(dmzap->dev, "Zone %d assigned to class 0.\n", zone->seq);
}

/*
 * Remove zone from the class 0 list
 */
static void dmzap_unassign_zone_from_class_0(struct dmzap_target *dmzap,
  struct dmzap_zone *zone)
{
	//trace_printk("Unassign zone %d from class 0.\n", zone->seq);
  if(dmzap->nr_reclaim_class_0 <= 0){
    dmz_dev_err(dmzap->dev, "Reclaim class 0 is out of bounds: %d.\n", dmzap->nr_reclaim_class_0);
    //BUG();
    return;
  }

  list_del_init(&zone->link);
  dmzap->nr_reclaim_class_0--;
	zone->reclaim_class = -1;
	if(dmzap->show_debug_msg)
  	dmz_dev_debug(dmzap->dev, "Zone %d unassigned from class 0.\n", zone->seq);
}

/*
 *  Adjust the threshold_cb, so that class_0 is populated with 0 - dmzap->class_0_cap members
 */
static inline void dmzap_ajust_threshold_cb(struct dmzap_target *dmzap)
{
  int i = 0;
  struct dmzap_zone *zone;
  unsigned long currentTime = jiffies;
  int new_threshold_cb_index = dmzap->nr_internal_zones - dmzap->class_0_optimal;
	int zone_reclaim_class = -1;
	unsigned long flags;
  if(new_threshold_cb_index < 0){
    dmz_dev_err(dmzap->dev, "dmzap->class_0_optimal is bigger than the number of internal zones. %d \n", new_threshold_cb_index);
    //BUG();
    return;
  }

	spin_lock_irqsave(&dmzap->debug_lock, flags);
	trace_printk("Adjusting threshold, nrClass0: %d, nrClass1: %d.\n",
		dmzap->nr_reclaim_class_0,
		dmzap->nr_reclaim_class_1);


  for(i = 0; i < dmzap->nr_internal_zones; i++){
    zone = &dmzap->dmzap_zones[i];
    if(zone->zone->cond == BLK_ZONE_COND_FULL){
      zone->cb = dmzap_calc_cb_value(dmzap, zone, currentTime);
			zone_reclaim_class = zone->reclaim_class;
			if (zone_reclaim_class == 1) {
				dmzap_unassign_zone_from_class_1(dmzap,zone);
			} else if(zone_reclaim_class == 0){
				dmzap_unassign_zone_from_class_0(dmzap,zone);
			} else {
				dmz_dev_err(dmzap->dev, "Zone %d should be already assigned to a class.\n", zone->seq);
			}
    } else {
      zone->cb = 0;
    }
    dmzap->reclaim->cb[i] = zone->cb;
  }

  list_del_init(&dmzap->reclaim_class_0);

  sort(dmzap->reclaim->cb, dmzap->nr_internal_zones, sizeof(long long), &compare, NULL);
  dmzap->threshold_cb = dmzap->reclaim->cb[new_threshold_cb_index];

	//Assign all full zones to new zone.
  for(i = 0; i < dmzap->nr_internal_zones; i++){
    zone = &dmzap->dmzap_zones[i];
    if(zone->zone->cond == BLK_ZONE_COND_FULL){
      if(zone->cb > dmzap->threshold_cb){
        dmzap_assign_zone_to_class_0(dmzap, zone);
      } else {
        dmzap_calc_shift_time(dmzap, zone, zone->cb, currentTime);
				dmzap_rb_insert_zone_class_1(dmzap,zone);
      }
    }
  }
	trace_printk("After adjusting threshold, nrClass0: %d, nrClass1: %d.\n",
		dmzap->nr_reclaim_class_0,
		dmzap->nr_reclaim_class_1);

	spin_unlock_irqrestore(&dmzap->debug_lock, flags);
}

/*
 * Invalidate all the blocks in the range [block..block+nr_blocks-1].
 */
int dmzap_invalidate_blocks(struct dmzap_target *dmzap,
  sector_t backing_block,
  unsigned int nr_blocks)
{
  sector_t current_block;
  struct dmzap_zone *current_zone;
  unsigned long currentTime;
  long long cb;
	int zone_reclaim_class = -1;
	unsigned long flags;
  u64 start_ns;

  if(dmzap->show_debug_msg){
    dmz_dev_debug(dmzap->dev, "Invalidate backing_block %llu, %u blocks",
            (u64)backing_block, nr_blocks);
  }

  WARN_ON(backing_block + nr_blocks > dmz_sect2blk(dmzap->dev->zone_nr_sectors) * dmzap->dev->nr_zones);

  while (nr_blocks) {
    current_block = backing_block + (nr_blocks-1);
    if (dmzap->map.invalid_device_block[current_block] == 0) {
      dmzap->map.invalid_device_block[current_block] = 1;
      current_zone = &dmzap->dmzap_zones[dmzap_block2zone_id(dmzap,current_block)];
      current_zone->nr_invalid_blocks++;
			currentTime = jiffies;
			current_zone->zone_age = currentTime;
      if (dmzap->victim_selection_method == DMZAP_FAGCPLUS) {
				start_ns = ktime_get_ns();
				current_zone->cps += dmzap->current_write_num - dmzap->fagc_cps[current_block];
				if (current_zone->zone->cond == BLK_ZONE_COND_FULL){
					//dmz_dev_info(dmzap->dev, "zone: %lu", current_zone->seq);
					dmzap_heap_update(&dmzap->fagc_heap, current_zone->fegc_heap_pos);
				}
				dmzap->fagc_cps[current_block] = dmzap->current_write_num;
				// dmzap->gc_process_time +=  ktime_get_ns() - start_ns;
				// dmzap->gc_process_count++;
			}

      if (current_zone->zone->cond == BLK_ZONE_COND_FULL &&  (dmzap->victim_selection_method == DMZAP_CONST_GREEDY || dmzap->victim_selection_method == DMZAP_CONST_CB)){
            //dmz_dev_debug(dmzap->dev, "Updating constant time lists for zone %u with %d invalid blocks", current_zone->seq, current_zone->nr_invalid_blocks);

            // list_for_each_entry(zone_iterator, &(dmzap->num_invalid_blocks_lists[current_zone->nr_invalid_blocks-1]), num_invalid_blocks_link){
            // 	if (zone_iterator == current_zone){
            // 		found = true;
            // 		break;
            // 	}
            // }
            // BUG_ON(!found);
            list_del(&(current_zone->num_invalid_blocks_link));
            list_add_tail(&(current_zone->num_invalid_blocks_link), &(dmzap->num_invalid_blocks_lists[current_zone->nr_invalid_blocks]));
      }

      if (current_zone->zone->cond == BLK_ZONE_COND_FULL &&
            dmzap->victim_selection_method == DMZAP_FAST_CB) {

				spin_lock_irqsave(&dmzap->debug_lock, flags);
        //Update the shift time
				cb = dmzap_calc_cb_value(dmzap, current_zone, currentTime);
				zone_reclaim_class = current_zone->reclaim_class;
			  if(zone_reclaim_class == 0){
					// trace_printk("Put zone from class 0 to 1 after invalidation, nrClass0: %d, nrClass1: %d.\n",
					// 	dmzap->nr_reclaim_class_0,
					// 	dmzap->nr_reclaim_class_1);
					dmzap_calc_shift_time(dmzap,current_zone, cb, currentTime);

					if(!(dmzap->nr_reclaim_class_1 == 0 &&
					      dmzap->nr_reclaim_class_0 < dmzap->class_0_cap) ||
								!(current_zone->shift_time - DMZAP_CLASS_0_DELTA_PERIOD <= currentTime)){

						trace_printk("Put zone from class 0 to 1 after invalidation, nrClass0: %d, nrClass1: %d.\n",
							dmzap->nr_reclaim_class_0,
							dmzap->nr_reclaim_class_1);
						dmzap_unassign_zone_from_class_0(dmzap, current_zone);
						dmzap_rb_insert_zone_class_1(dmzap,current_zone);
					}
					//class 0 does not have to be reassigned because it is not sorted.

			  } else if(zone_reclaim_class == 1){
					// trace_printk("Reorganize zone from class 1 after invalidation, nrClass0: %d, nrClass1: %d.\n",
					// 	dmzap->nr_reclaim_class_0,
					// 	dmzap->nr_reclaim_class_1);
					dmzap_unassign_zone_from_class_1(dmzap, current_zone);
					dmzap_calc_shift_time(dmzap,current_zone, cb, currentTime);
					dmzap_rb_insert_zone_class_1(dmzap,current_zone);
				} else {
					printk("IN invalidation class %d, seq %d", zone_reclaim_class, current_zone->seq);
					// BUG();
				}
				spin_unlock_irqrestore(&dmzap->debug_lock, flags);
      }
    }
    nr_blocks--;
  }
  //TODO invalidate the persistent mapping
  return 0;
}

/*
 * Validate all the blocks in the range [block..block+nr_blocks-1].
 */
int dmzap_validate_blocks(struct dmzap_target *dmzap,
  sector_t backing_block,
  unsigned int nr_blocks)
{
  sector_t current_block;
	struct dmzap_zone *zone_iterator;
	struct dmzap_zone *current_zone;
	bool found = false;
    unsigned int current_zone_index;

  if(dmzap->show_debug_msg){
    dmz_dev_debug(dmzap->dev, "Validate backing_block %llu, %u blocks",
            (u64)backing_block, nr_blocks);
  }

  WARN_ON(backing_block + nr_blocks > dmz_sect2blk(dmzap->dev->zone_nr_sectors) * dmzap->dev->nr_zones);

  while (nr_blocks) {
      current_block = backing_block + (nr_blocks-1);
      if (dmzap->map.invalid_device_block[current_block] == 1) {
          dmzap->map.invalid_device_block[current_block] = 0;
          current_zone_index = dmzap_block2zone_id(dmzap,current_block);
          dmzap->dmzap_zones[current_zone_index].nr_invalid_blocks--;
          current_zone = &dmzap->dmzap_zones[current_zone_index];
          if (current_zone->zone->cond == BLK_ZONE_COND_FULL && (dmzap->victim_selection_method == DMZAP_CONST_GREEDY || dmzap->victim_selection_method == DMZAP_CONST_CB)){
              //dmz_dev_debug(dmzap->dev, "Updating constant time lists for zone %u with %d invalid blocks", current_zone->seq, current_zone->nr_invalid_blocks);


              // list_for_each_entry(zone_iterator, &(dmzap->num_invalid_blocks_lists[current_zone->nr_invalid_blocks+1]), num_invalid_blocks_link){
              // 	if (zone_iterator == current_zone){
              // 		found = true;
              // 		break;
              // 	}
              // }
              // BUG_ON(!found);


              list_del(&(current_zone->num_invalid_blocks_link));
              list_add_tail(&(current_zone->num_invalid_blocks_link), &(dmzap->num_invalid_blocks_lists[current_zone->nr_invalid_blocks]));
          }
      }
      nr_blocks--;
  }
  //TODO validate the persistent mapping
  return 0;
}

/*
 * Cost benefit value calculation
 */
long long dmzap_calc_cb_value(struct dmzap_target *dmzap,
  const struct dmzap_zone *zone, unsigned long currentTime)
{
  long long cb = 0;
  int scale_factor = DMZAP_CB_SCALE_FACTOR;
  unsigned long ab = currentTime - zone->zone_age;
  int nr_invalid_blocks = zone->nr_invalid_blocks;
  int nr_valid_blocks = dmz_sect2blk(dmzap->dev->zone_nr_sectors) - nr_invalid_blocks; //TODO ZNS capacity: int nr_valid_blocks = dmz_sect2blk(victim->zone->capacity) - nr_invalid_blocks;
	/* Do not devide by zero */
	if (nr_valid_blocks == 0){
		cb =  ab * (nr_invalid_blocks) * scale_factor;
	} else {
		cb =  ab * (nr_invalid_blocks) * scale_factor / (2 * nr_valid_blocks);
	}
  if(dmzap->show_debug_msg)
    dmz_dev_debug(dmzap->dev, "CB value %lld for zone %d.\n", cb, zone->seq);
  return cb;
}

/*
 * Initially decide to which class the zone is assigned
 */
void dmzap_assign_zone_to_reclaim_class(struct dmzap_target *dmzap,
  struct dmzap_zone *zone)
{
  //TODO also calc new threshold here?
  unsigned long currentTime = jiffies;
  long long cb = dmzap_calc_cb_value(dmzap, zone, currentTime);
	unsigned long flags;

	spin_lock_irqsave(&dmzap->debug_lock, flags);
  zone->cb = cb;

	if(zone->reclaim_class != -1){
		printk("RECLAIM CLASS IS ALREADY SET");
	}

  if(cb > dmzap->threshold_cb ||
        (dmzap->nr_reclaim_class_1 == 0 &&
        dmzap->nr_reclaim_class_0 < dmzap->class_0_cap)){
		// trace_printk("Initially assigning zone to class 0, nrClass0: %d, nrClass1: %d.\n",
		// 	dmzap->nr_reclaim_class_0,
		// 	dmzap->nr_reclaim_class_1);
    dmzap_assign_zone_to_class_0(dmzap, zone);
  } else {
		// trace_printk("Initially assigning zone to class 1, nrClass0: %d, nrClass1: %d.\n",
		// 	dmzap->nr_reclaim_class_0,
		// 	dmzap->nr_reclaim_class_1);
    dmzap_calc_shift_time(dmzap, zone, cb, currentTime);
		dmzap_rb_insert_zone_class_1(dmzap,zone);
  }
	spin_unlock_irqrestore(&dmzap->debug_lock, flags);
}

/*
 * Selecting the zone, that will be freed
 */
struct dmzap_zone * dmzap_cb_victim_selection(struct dmzap_target *dmzap)
{
  struct dmzap_zone *victim = NULL;
	struct dmzap_zone *zone = NULL;
  long long highest_cb = -1;
  long seq_id = -1;
  long i = 0;
  long long current_cb = 0;
  unsigned long currentTime = jiffies;

  for(i = 0; i < dmzap->nr_internal_zones; i++){
		zone = &dmzap->dmzap_zones[i];
		if(zone->zone->cond == BLK_ZONE_COND_FULL && zone->nr_invalid_blocks != 0){
	    current_cb = dmzap_calc_cb_value(dmzap, zone, currentTime);
	    if(current_cb > highest_cb){
	      seq_id = i;
	      highest_cb = current_cb;
	    }
		}
  }

  if(seq_id == -1 || dmzap->dmzap_zones[seq_id].nr_invalid_blocks == 0){
    return NULL;
  }

  victim = &dmzap->dmzap_zones[seq_id];
  return victim;
}


struct dmzap_zone * dmzap_approx_cb_victim_selection (struct dmzap_target *dmzap)
{
	struct dmzap_zone *victim = NULL;
	struct dmzap_zone *zone = NULL;
	long long current_cb = 0;
	long long highest_cb = -1;
	long seq_id = -1;
	u32 i = 0;
	int new_threshold_cb_index = dmzap->nr_internal_zones - dmzap->q_cap;
	unsigned long currentTime = jiffies;

	if(new_threshold_cb_index < 0)
		new_threshold_cb_index = 0;


	if(dmzap->q_length == 0){
		//Calc for all zones CB value
		for(i = 0; i < dmzap->nr_internal_zones; i++){
			zone = &dmzap->dmzap_zones[i];
			if(zone->zone->cond == BLK_ZONE_COND_FULL){
				zone->cb = dmzap_calc_cb_value(dmzap, zone, currentTime);
				if(zone->cb > highest_cb){
					seq_id = i;
					highest_cb = zone->cb;
				}
			} else {
	      zone->cb = 0;
	    }
			dmzap->reclaim->cb[i] = zone->cb;
	  }

		//Calc theshold so that zones over that threshold are assigned to q
	  sort(dmzap->reclaim->cb, dmzap->nr_internal_zones, sizeof(long long), &compare, NULL);
	  dmzap->threshold_cb = dmzap->reclaim->cb[new_threshold_cb_index];

		list_del_init(&dmzap->q_list);

		for(i = 0; i < dmzap->nr_internal_zones; i++){
			zone = &dmzap->dmzap_zones[i];
			if(zone->zone->cond == BLK_ZONE_COND_FULL &&
						zone->cb >= dmzap->threshold_cb &&
						zone->nr_invalid_blocks != 0 &&
						dmzap->q_length <= dmzap->q_cap){

				list_add_tail(&zone->link, &dmzap->q_list);
				dmzap->q_length++;
			}
	  }
  } else {
		// find highest cb value from queue
		list_for_each_entry(zone, &dmzap->q_list, link) {
			if(zone->zone->cond == BLK_ZONE_COND_FULL){
				current_cb = dmzap_calc_cb_value(dmzap, zone, currentTime);
				if(current_cb > highest_cb){
					seq_id = zone->seq;
					highest_cb = current_cb;
				}
			}
		}
	}


  if(seq_id == -1 || dmzap->dmzap_zones[seq_id].nr_invalid_blocks == 0){
    return NULL;
  }

  victim = &dmzap->dmzap_zones[seq_id];
	list_del_init(&victim->link);
	dmzap->q_length--;

	return victim;
}


/*
 * Return zone, which has to be shifted from class 1 to 0 based on the shift time
 */
static struct dmzap_zone *dmzap_zone_from_class_1_to_transfer(struct dmzap_target *dmzap,
					      unsigned long current_time)
{
	struct dmzap_zone *zone = NULL;
	struct rb_node *node = NULL;
	node = rb_first(&dmzap->reclaim_class_1_rbtree);
	if(node){
		zone = rb_entry(node, struct dmzap_zone, node);
		if(zone && zone->shift_time <= current_time){
			return zone;
		}
	}

	return NULL;
}

/*
 * Selecting the zone, that will be freed
 */
struct dmzap_zone * dmzap_fast_cb_victim_selection(struct dmzap_target *dmzap)
{
  struct dmzap_zone *victim = NULL; //&dmzap->dmzap_zones[0];
  struct dmzap_zone *current_zone;
  long long biggest_cb = -1;
  long long current_cb = 0;
  unsigned long current_time = jiffies;
	struct dmzap_zone *zone_to_shift = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dmzap->debug_lock, flags);
  if(dmzap->nr_reclaim_class_0 == 0 && dmzap->nr_reclaim_class_1 == 0){
		spin_unlock_irqrestore(&dmzap->debug_lock, flags);
    return NULL;
  }
	spin_unlock_irqrestore(&dmzap->debug_lock, flags);


	do {
		spin_lock_irqsave(&dmzap->debug_lock, flags);
		zone_to_shift = NULL;
		zone_to_shift = dmzap_zone_from_class_1_to_transfer(dmzap,current_time);
		if(zone_to_shift){
			if(zone_to_shift->reclaim_class == 0){
				printk("BUG! zone %d", zone_to_shift->seq);
				printk("class_0 %d class_1 %d",dmzap->nr_reclaim_class_0, dmzap->nr_reclaim_class_1);
			}
			trace_printk("Shift_time for a zone reached, nrClass0: %d, nrClass1: %d.\n",
				dmzap->nr_reclaim_class_0,
				dmzap->nr_reclaim_class_1);
			dmzap_unassign_zone_from_class_1(dmzap,zone_to_shift);
			dmzap_assign_zone_to_class_0(dmzap, zone_to_shift);
		}
		spin_unlock_irqrestore(&dmzap->debug_lock, flags);
	} while(zone_to_shift);

	spin_lock_irqsave(&dmzap->debug_lock, flags);
  if( (dmzap->nr_reclaim_class_0 <= 0 && dmzap->nr_reclaim_class_1 > 0) ||
        dmzap->nr_reclaim_class_0 > dmzap->class_0_cap){
		spin_unlock_irqrestore(&dmzap->debug_lock, flags);

    dmzap_ajust_threshold_cb(dmzap);
  }
	spin_unlock_irqrestore(&dmzap->debug_lock, flags);

	spin_lock_irqsave(&dmzap->debug_lock, flags);
  list_for_each_entry(current_zone, &dmzap->reclaim_class_0, link){
		if(current_zone->nr_invalid_blocks != 0){
			current_cb = dmzap_calc_cb_value(dmzap, current_zone, current_time);
			if(current_cb > biggest_cb){
				biggest_cb = current_cb;
				victim = current_zone;
			}
		}
  }
	spin_unlock_irqrestore(&dmzap->debug_lock, flags);

  if(victim && victim->nr_invalid_blocks == 0){
    return NULL;
  }

	if(victim){
		victim->zone->cond = BLK_ZONE_COND_OFFLINE; //So the victim is not getting assigned to class 0 in the mean time
	}
  return victim;
}

/*
 * Selecting the zone, that will be freed. Greedy victim selection
 */
struct dmzap_zone * dmzap_victim_selection(struct dmzap_target *dmzap)
{
  struct dmzap_zone *victim = &dmzap->dmzap_zones[0];
  int i = 0;
  int max_invalid_blocks = 0;
  int victim_index = -1;
  int tmp = 0;
  for (i = 0; i < dmzap->nr_internal_zones; i++) {
    if (dmzap->dmzap_zones[i].zone->cond == BLK_ZONE_COND_CLOSED ||
      dmzap->dmzap_zones[i].zone->cond == BLK_ZONE_COND_FULL) {
        tmp = 0;
        tmp =  dmzap->dmzap_zones[i].nr_invalid_blocks;

        if(tmp > max_invalid_blocks) {
          max_invalid_blocks = tmp;
          victim_index = i;
        }
    }
  }

  if(victim_index == -1 || max_invalid_blocks == 0){
    return NULL;
  }
  victim = &dmzap->dmzap_zones[victim_index];
  return victim;
}

/*
 * Selecting the zone, that will be freed. FeGC selection
 */
struct dmzap_zone *dmzap_fegc_victim_selection(struct dmzap_target *dmzap)
{
	struct dmzap_zone *victim = NULL;
	int i = 0, j = 0, bug_found = 0;
	int max_invalid_blocks = 0;
	unsigned long long victim_cwa = 0;
	struct dmzap_fegc_heap *victim_heap = NULL;
	int heap_index = -1;
	int tmp = 0;
	u64 jiff = jiffies;
	for (i = dmz_sect2blk(dmzap->dev->zone_nr_sectors); i > 0; i--) {
		if (dmzap->fegc_heaps[i]->size == 0) {
			continue;
		}
		// if (dmzap->fegc_heaps[i]->size > (9*dmzap->fegc_heaps[i]->max_size)/10){
		// 	dmzap_heap_increase_size(dmzap->fegc_heaps[i]);
		// }
		// dmz_dev_debug(dmzap->dev,
		// 	      "Searching heap %d [%px] with cwa: %llu, seq: %u",
		// 	      i, &dmzap->fegc_heaps[i],
		// 	      dmzap->fegc_heaps[i].data[1]->cwa,
		// 	      dmzap->fegc_heaps[i].data[1]->seq);
		if (updated_cwa(dmzap->fegc_heaps[i]->data[1], jiff) > victim_cwa) {
			victim = dmzap->fegc_heaps[i]->data[1];
			victim_cwa = updated_cwa(victim, jiff);
			victim_heap = dmzap->fegc_heaps[i];
			heap_index = i;
		}

		
	}
	//dmz_dev_info(dmzap->dev, "Selected zone %u for eviction with %d invalid blocks from: %px", victim->seq, victim->nr_invalid_blocks, victim_heap);
    //assert_heap_ok(dmzap, 6);
    if (victim == NULL){
	//     for (i = dmzap->dev->zone_nr_blocks; i > 0; i--) {
	//     	dmz_dev_info(dmzap->dev, "Heap: %d: size: %u", i, dmzap->fegc_heaps[i]->size);
	//     }
	return victim;
    }
	BUG_ON(victim == NULL);

//     for (i = 1; i <= dmzap->fegc_heaps[heap_index].size; i++) {
// 		dmz_dev_info(dmzap->dev, "Heap: %d, item %i, seq: %u, cwa: %llu", heap_index, i, dmzap->fegc_heaps[heap_index].data[i]->seq, dmzap->fegc_heaps[heap_index].data[i]->cwa);
// 	}

	dmzap_heap_delete(victim_heap, victim_heap->data[1]);
// 	dmz_dev_info(dmzap->dev, "New head: %u, victim: %u",  dmzap->fegc_heaps[heap_index].data[1]->seq, victim->seq);
//     for (i = 1; i <= dmzap->fegc_heaps[heap_index].size; i++) {
// 		dmz_dev_debug(dmzap->dev, "Heap: %d, item %i, seq: %u, cwa: %llu", heap_index, i, dmzap->fegc_heaps[heap_index].data[i]->seq, dmzap->fegc_heaps[heap_index].data[i]->cwa);
// 	}
// 	if (!heaps_are_ok(dmzap)){
// 		dmz_dev_info(dmzap->dev, "After deleting zone %u to heap %d", victim->seq, heap_index);
// 		heap_print(dmzap, heap_index);
// 		BUG();
// 	}
//     assert_heap_ok(dmzap, 5);

	// dmz_dev_debug(
	// 	dmzap->dev,
	// 	"Selected zone %u for eviction with %d invalid blocks from heap %d",
	// 	victim->seq, victim->nr_invalid_blocks, heap_index);

	

	victim->cwa = 0;
	victim->cwa_time = 0;
	//dmzap->nr_user_written_sec = 0; 
	return victim;
}

/*
 * Selecting the zone, that will be freed. FaGC+ selection
 */
struct dmzap_zone *dmzap_fagcplus_victim_selection(struct dmzap_target *dmzap)
{
	struct dmzap_zone *victim = NULL;
	int i = 0, j = 0, bug_found = 0;
	int max_invalid_blocks = 0;
	unsigned long long victim_cwa = 0;
	int tmp = 0;
	u64 jiff = jiffies;
	
	victim = dmzap->fagc_heap.data[1];

	dmzap_heap_delete(&dmzap->fagc_heap, victim);

	victim->cps = 0;
	victim->fegc_heap_pos = -1;
	//dmz_dev_info(dmzap->dev, "victim: %lu", victim->seq);
	return victim;
}

/*
 * Selecting the zone, that will be freed. Constant time greedy victim selection
 */
struct dmzap_zone *
dmzap_const_greedy_victim_selection(struct dmzap_target *dmzap)
{
	struct dmzap_zone *victim = &dmzap->dmzap_zones[0];
	int i = 0;
	int max_invalid_blocks = 0;

	for (i = dmz_sect2blk(dmzap->dev->zone_nr_sectors); i > 0; i--) {
		if (list_empty(&dmzap->num_invalid_blocks_lists[i])) {
			continue;
		}
		victim = list_first_entry(&dmzap->num_invalid_blocks_lists[i],
					  struct dmzap_zone,
					  num_invalid_blocks_link);
		list_del(&victim->num_invalid_blocks_link);
		BUG_ON(victim->zone->cond != BLK_ZONE_COND_CLOSED &&
		        victim->zone->cond != BLK_ZONE_COND_FULL);
		//dmz_dev_debug(dmzap->dev, "Selected zone %p for eviction with %d invalid blocks", victim, victim->nr_invalid_blocks);
		return victim;
	}
	dmz_dev_info(
		dmzap->dev,
		"No block has been selected for eviction, all lists are empty!");
	return NULL;
}

/*
 * Selecting the zone, that will be freed. Constant time cost-benefit victim selection
 */
struct dmzap_zone *dmzap_const_cb_victim_selection(struct dmzap_target *dmzap)
{
	struct dmzap_zone *victim = &dmzap->dmzap_zones[0];
	int i = 0;
	int max_invalid_blocks = 0;
	long long max_benefit = -1;
	long long current_benefit;
	struct dmzap_zone *current_victim = NULL;

	for (i = dmz_sect2blk(dmzap->dev->zone_nr_sectors)/4; i > 0; i--) {
		if (list_empty(&dmzap->num_invalid_blocks_lists[i])) {
			continue;
		}
		victim = list_first_entry(&dmzap->num_invalid_blocks_lists[i],
					  struct dmzap_zone,
					  num_invalid_blocks_link);
		// BUG_ON(victim->zone->cond != BLK_ZONE_COND_CLOSED &&
		//        victim->zone->cond != BLK_ZONE_COND_FULL);
		current_benefit = dmzap_calc_cb_value(dmzap, victim, jiffies);
		if (current_benefit > max_benefit) {
			max_benefit = current_benefit;
			current_victim = victim;
		}
	}
	// dmz_dev_debug(
	// 	dmzap->dev,
	// 	"Zone %u has been selected for eviction, all lists are empty!",
	// 	current_victim ? current_victim->seq : -1);
	list_del(&current_victim->num_invalid_blocks_link);
	return current_victim;
}

/*
 * Copy valid blocks of victim_zone into free zone.
 */
void dmzap_copy_valid_data(struct dmzap_target *dmzap,
        struct dmzap_zone *victim_zone)
{
  sector_t chunk_block = 0;
  sector_t read_block = 0;
  sector_t write_block = 0;
  int invalid_flag = 0;
  struct dmzap_zone *fresh_zone;
  unsigned long flags = 0;
  struct dm_io_region src, dst;
  sector_t nr_chunk_blocks = 0;
  sector_t i = 0;
  __u64 fresh_zone_block_length = 0;
  sector_t zone_block_length = dmz_sect2blk(dmzap->dev->zone_nr_sectors); //TODO ZNS capacity: sector_t zone_block_length = dmz_sect2blk(victim->zone->capacity);

  /* We can only have one outstanding write at a time */
	while(test_and_set_bit_lock(DMZAP_WR_OUTSTANDING,
				&dmzap->write_bitmap))
		io_schedule();

  set_bit(DM_KCOPYD_WRITE_SEQ, &flags);

  while (chunk_block < zone_block_length) {
    nr_chunk_blocks = 1;

    invalid_flag = dmzap_get_invalid_flag(dmzap,victim_zone,chunk_block);

    if(!invalid_flag){
      nr_chunk_blocks = -1;

      for(i = chunk_block + 1; i < zone_block_length; i++){
        invalid_flag = dmzap_get_invalid_flag(dmzap,victim_zone,i);
        if(invalid_flag){
          nr_chunk_blocks = i - chunk_block;
          break;
        }
      }

      if(nr_chunk_blocks == -1){
        nr_chunk_blocks = zone_block_length - chunk_block;
      }

      fresh_zone = &dmzap->dmzap_zones[dmzap->dmzap_zone_wp];

      fresh_zone_block_length =
        dmz_sect2blk(fresh_zone->zone->start + fresh_zone->zone->len) //TODO ZNS capacity: dmz_sect2blk(fresh_zone->zone->start + fresh_zone->zone->capacity)
        - dmz_sect2blk(fresh_zone->zone->wp);

      if(fresh_zone_block_length < nr_chunk_blocks){
        nr_chunk_blocks = fresh_zone_block_length;
      }

      read_block = dmz_sect2blk(victim_zone->zone->start) + chunk_block;
      write_block = dmz_sect2blk(fresh_zone->zone->wp);
      if(dmzap->show_debug_msg){
        dmz_dev_debug(dmzap->dev, "Copying valid block %lld to block %lld.\n",
          read_block, write_block);
      }

      src.bdev = dmzap->dev->bdev;
      src.sector = dmz_blk2sect(read_block);
      src.count = dmz_blk2sect(nr_chunk_blocks);

      dst.bdev = dmzap->dev->bdev;
      dst.sector = dmz_blk2sect(write_block);
      dst.count = src.count;


      /* Copy the valid region */
      set_bit(DMZAP_RECLAIM_KCOPY, &dmzap->reclaim->flags);
      dm_kcopyd_copy(dmzap->reclaim->kc, &src, 1, &dst, flags,
               dmzap_reclaim_kcopy_end, dmzap->reclaim);

			dmzap_remap_copy(dmzap,	read_block, write_block, nr_chunk_blocks);

      /* Wait for copy to complete */
      wait_on_bit_io(&dmzap->reclaim->flags, DMZAP_RECLAIM_KCOPY,
               TASK_UNINTERRUPTIBLE);
      if (dmzap->reclaim->kc_err){
        dmz_dev_err(dmzap->dev, "COPY error %d.\n", dmzap->reclaim->kc_err);
        return;
      }

      dmzap->nr_gc_written_sec += dmz_blk2sect(nr_chunk_blocks);

      dmzap_update_seq_wp(dmzap, dmz_blk2sect(nr_chunk_blocks));
    }

    chunk_block += nr_chunk_blocks;
  }

	clear_bit_unlock(DMZAP_WR_OUTSTANDING, &dmzap->write_bitmap);
}

int dmzap_reset_zone (struct dmzap_target *dmzap, struct dmzap_zone *victim)
{
  struct dmz_dev *dev = dmzap->dev;
	unsigned long flags;
  // __u8 cond = victim->zone->cond;
  // __u8 type = victim->zone->type;
  int ret = 0;

  // if (cond == BLK_ZONE_COND_OFFLINE ||
  //     cond == BLK_ZONE_COND_READONLY ||
  //     type == BLK_ZONE_TYPE_CONVENTIONAL) return 0;

  ret = blkdev_zone_mgmt(dev->bdev, REQ_OP_ZONE_RESET,
             victim->zone->start, dev->zone_nr_sectors, GFP_NOIO); //TODO ZNS capacity Not sure about that (maybe the line can stay as it is): victim->zone->start, victim->zone->capacity, GFP_NOIO);

  if (ret) {
    dmz_dev_err(dev, "Reset zone %u failed %d",
          victim->seq, ret);
    return ret;
  }

  if(dmzap->victim_selection_method == DMZAP_FAST_CB){
		//printk("victim class %d for victim %d", victim->reclaim_class, victim->seq);
		spin_lock_irqsave(&dmzap->debug_lock, flags);
    dmzap_unassign_zone_from_class_0(dmzap, victim);
		spin_unlock_irqrestore(&dmzap->debug_lock, flags);
  }

  victim->zone->wp = victim->zone->start;
  victim->zone->cond = BLK_ZONE_COND_EMPTY;

  return 0;
}

/*
 *
 */
int dmzap_free_victim (struct dmzap_target *dmzap, struct dmzap_zone *victim)
{
  int ret = 0;
  int i = 0;
  sector_t victim_start_block = dmz_sect2blk(victim->zone->start);
  int free_zones = 0;

  /* We can only have one outstanding write at a time */
  while(test_and_set_bit_lock(DMZAP_WR_OUTSTANDING,
        &dmzap->write_bitmap))
    io_schedule();

  //TODO delete, just for debugging purpose
  for(i = 0; i < dmzap->nr_internal_zones; i++){
    if(dmzap->dmzap_zones[i].zone->cond == BLK_ZONE_COND_EMPTY){
      free_zones++;
    }
  }

	if(dmzap->show_debug_msg)
	  dmz_dev_debug(dmzap->dev,
	    "Freeing victim. Sequence id %d. Start sector %lld. Ammount of sectors %lld. Ammount of free zones %d.",
	    victim->seq,
	    victim->zone->start,
	    victim->zone->len,
	    free_zones);

  ret = dmzap_reset_zone(dmzap, victim);

  if (ret) {
   dmz_dev_err(dmzap->dev, "Reset zone %u failed %d",
         victim->seq, ret);
   goto out;
  }

  victim->zone_age = jiffies;

  dmzap_validate_blocks(dmzap, victim_start_block, dmz_sect2blk(dmzap->dev->zone_nr_sectors));
  dmzap_unmap_zone_entries(dmzap, victim);
	dmzap->reclaim->nr_free_zones++;
	dmzap_calc_p_free_zone(dmzap);
	trace_printk("+Number of free zones %lu. (total %u)\n", dmzap->reclaim->nr_free_zones, dmzap->nr_internal_zones);


out:
  clear_bit_unlock(DMZAP_WR_OUTSTANDING, &dmzap->write_bitmap);
  return ret;
}

/*
 * Reclaim procedure
 */
static int dmzap_do_reclaim(struct dmzap_target *dmzap)
{
    struct dmzap_zone *victim = NULL;
    unsigned long start = jiffies;
		int nr_invalid_blocks = 0;
    int ret = 0;

    if(dmzap->victim_selection_method == DMZAP_GREEDY){
      victim = dmzap_victim_selection(dmzap);
    } else if(dmzap->victim_selection_method == DMZAP_CB){
      victim = dmzap_cb_victim_selection(dmzap);
    } else if(dmzap->victim_selection_method == DMZAP_FAST_CB){
      victim = dmzap_fast_cb_victim_selection(dmzap);
    } else if (dmzap->victim_selection_method == DMZAP_APPROX_CB) {
			victim = dmzap_approx_cb_victim_selection(dmzap);
		} else if (dmzap->victim_selection_method == DMZAP_CONST_GREEDY) {
		victim = dmzap_const_greedy_victim_selection(dmzap);
    } else if (dmzap->victim_selection_method == DMZAP_CONST_CB) {
      victim = dmzap_const_cb_victim_selection(dmzap);
    } else if (dmzap->victim_selection_method == DMZAP_FEGC) {
		victim = dmzap_fegc_victim_selection(dmzap);
    } else if (dmzap->victim_selection_method == DMZAP_FAGCPLUS) {
      victim = dmzap_fagcplus_victim_selection(dmzap);
    }

    if (victim) {
			trace_printk("Victim selected in %d ms, with %d invlaid blocks. Free zones: %lu. vsm: %u, dev_c: %lld\n",
						jiffies_to_msecs(jiffies - start),
						victim->nr_invalid_blocks,
					 	dmzap->reclaim->nr_free_zones,
						dmzap->victim_selection_method,
						dmzap->dev_capacity);
			if(dmzap->show_debug_msg)
      	dmz_dev_debug(dmzap->dev, "About to free victim zone %d, with %d invalid blocks (%lld valid blocks).",
					victim->seq,
					victim->nr_invalid_blocks,
					dmz_sect2blk(dmzap->dev->zone_nr_sectors) - victim->nr_invalid_blocks);
			nr_invalid_blocks = victim->nr_invalid_blocks;
			dmzap_copy_valid_data(dmzap, victim);
      dmzap_free_victim(dmzap, victim);
      //dmzap_reclaim_bio_acc(dmzap); // TODO Maybe leave it out?
			if(dmzap->show_debug_msg)
				dmz_dev_debug(dmzap->dev, "Reclaimed zone %d in %u ms, with %d invalid blocks",
              victim->seq, jiffies_to_msecs(jiffies - start), nr_invalid_blocks);

    } else {
			trace_printk("No victim selected in %d ms. vsm: %u, dev_c: %lld\n",
						jiffies_to_msecs(jiffies - start),
						dmzap->victim_selection_method,
						dmzap->dev_capacity);
      if(dmzap->show_debug_msg)
				dmz_dev_debug(dmzap->dev, "could not find victim");
    }

		// clear_bit_unlock(DMZAP_RECLAIM_KCOPY, &dmzap->flags);
		// smp_mb__after_atomic();
		// wake_up_bit(&dmzap->flags, DMZAP_RECLAIM_KCOPY);
    return ret;
}

static inline bool dmzap_target_idle(struct dmzap_target *dmzap)
{
  return time_is_before_jiffies(dmzap->reclaim->atime + DMZAP_IDLE_PERIOD);
}

/*
 * Trigger the reclaim
 */
static bool dmzap_should_reclaim(struct dmzap_target *dmzap)
{
  if (dmzap_target_idle(dmzap) && dmzap->reclaim->nr_free_zones
			< dmzap->nr_internal_zones)
    return true;

  //return dmzap->reclaim->p_free_user_zones <= dmzap->reclaim_limit;
	return dmzap->reclaim->p_free_zones <= dmzap->reclaim_limit;
}

/*
 * Reclaim work function.
 */
static void dmzap_reclaim_work(struct work_struct *work)
{
	struct dmzap_reclaim *reclaim = container_of(work, struct dmzap_reclaim, work.work);
    struct dmzap_target *dmzap = reclaim->dmzap;
	int ret;

	if (dmzap_bdev_is_dying(dmzap->dev))
		return;

	if (!dmzap_should_reclaim(dmzap)) {
		mod_delayed_work(reclaim->wq, &reclaim->work, DMZAP_IDLE_PERIOD);
		return;
	}

	/*
	 * We need to start reclaiming random zones: set up zone copy
	 * throttling to either go fast if we are very low on random zones
	 * and slower if there are still some free random zones to avoid
	 * as much as possible to negatively impact the user workload.
	 */
	if (dmzap_target_idle(dmzap) ||
				reclaim->p_free_zones <= dmzap->reclaim_limit ) {
				//reclaim->p_free_user_zones <= dmzap->reclaim_limit ) {
		/* Idle or very low percentage: go fast */
		reclaim->kc_throttle.throttle = 100;
	} else {
		/* Busy but we still have some random zone: throttle */
		reclaim->kc_throttle.throttle = min(75U, 100U - reclaim->p_free_user_zones / 2);
	}

  if (dmzap->show_debug_msg)
  	dmz_dev_debug(dmzap->dev,
  		      "Reclaim (%u): %s, %u%% free rnd zones (%ld/%u)",
  		      reclaim->kc_throttle.throttle,
  		      (dmzap_target_idle(dmzap) ? "Idle" : "Busy"),
  		      reclaim->p_free_zones, reclaim->nr_free_zones,
            dmzap->nr_internal_zones);

  mutex_lock(&dmzap->map.map_lock);
  ret = dmzap_do_reclaim(dmzap);
  mutex_unlock(&dmzap->map.map_lock);

  if (ret) {
		dmz_dev_debug(dmzap->dev, "Reclaim error %d\n", ret);
		if (!dmzap_check_bdev(dmzap->dev))
			return;
	}

	dmzap_schedule_reclaim(dmzap);
}

/*
 * Initialize reclaim.
 */
int dmzap_ctr_reclaim(struct dmzap_target *dmzap)
{
  struct dmzap_reclaim *reclaim;
  int ret = 0, i = 0;

  dmzap->reclaim = kzalloc(sizeof(struct dmzap_reclaim), GFP_KERNEL);
  if (!dmzap->reclaim)
    return -ENOMEM;

  reclaim = dmzap->reclaim;

  reclaim->cb = kvmalloc_array(dmzap->nr_internal_zones,
			sizeof(long long), GFP_KERNEL | __GFP_ZERO);
	if (!reclaim->cb){
    ret = -ENOMEM;
    goto err_reclaim;
  }

  mutex_init(&dmzap->reclaim_lock);
	spin_lock_init(&dmzap->debug_lock);
  INIT_LIST_HEAD(&dmzap->reclaim_class_0);
	INIT_LIST_HEAD(&dmzap->q_list);
	dmzap->reclaim_class_1_rbtree = RB_ROOT;

  /* Idle or very low percentage: go fast */
  reclaim->kc_throttle.throttle = 100;

  /* Reclaim kcopyd client */
	reclaim->kc = dm_kcopyd_client_create(&reclaim->kc_throttle);
	if (IS_ERR(reclaim->kc)) {
		ret = PTR_ERR(reclaim->kc);
		reclaim->kc = NULL;
		goto err;
	}

  /* Reclaim work */
	INIT_DELAYED_WORK(&reclaim->work, dmzap_reclaim_work);
	reclaim->wq = alloc_ordered_workqueue("dmzap_rwq_%s", WQ_MEM_RECLAIM,
					  dmzap->dev->name);
	if (!reclaim->wq) {
		ret = -ENOMEM;
		goto err;

	}
  if (dmzap->victim_selection_method == DMZAP_CONST_GREEDY || dmzap->victim_selection_method == DMZAP_CONST_CB){
		dmzap->num_invalid_blocks_lists = kzalloc(sizeof(struct list_head)* (dmzap->dev->zone_nr_sectors + 1), GFP_KERNEL);
		dmz_dev_debug(dmzap->dev, "Allocated %llu lists for constant time reclaims", (dmzap->dev->zone_nr_sectors + 1));

		for(i=0; i <= dmzap->dev->zone_nr_sectors;i++){
			INIT_LIST_HEAD(&dmzap->num_invalid_blocks_lists[i]);
		}
		dmz_dev_debug(dmzap->dev, "Initialized %llu lists for constant time reclaims", (dmzap->dev->zone_nr_sectors + 1));
	}

  if (dmzap->victim_selection_method == DMZAP_FEGC) {
		dmzap->fegc_heaps =	kzalloc(sizeof(struct dmzap_fegc_heap*) *	(dmz_sect2blk(dmzap->dev->zone_nr_sectors) + 1),	GFP_KERNEL);

		dmz_dev_info(dmzap->dev,
			      "Allocated %llu heaps for FeGC reclaim",
			      (dmz_sect2blk(dmzap->dev->zone_nr_sectors) + 1));

		// dmzap->fegc_heaps[0] = kzalloc(sizeof(struct dmzap_fegc_heap), GFP_KERNEL);
		// dmzap_fegc_heap_init(dmzap->fegc_heaps[0]);
		for (i = 0; i <= dmz_sect2blk(dmzap->dev->zone_nr_sectors); i++) {
			dmzap->fegc_heaps[i] = kzalloc(sizeof(struct dmzap_fegc_heap), GFP_KERNEL);
			dmzap_fegc_heap_init(dmzap->fegc_heaps[i]);
			//dmzap->fegc_heaps[i] = dmzap->fegc_heaps[0];
		}
		dmz_dev_info(dmzap->dev,
			      "Initialized %llu heaps for FeGC reclaim",
			      (dmz_sect2blk(dmzap->dev->zone_nr_sectors) + 1));
	}

	if (dmzap->victim_selection_method == DMZAP_FAGCPLUS) {
		dmzap->fagc_cps = vzalloc(sizeof(unsigned int)*dmzap->nr_internal_zones*dmz_sect2blk(dmzap->dev->zone_nr_sectors));
		dmz_dev_info(dmzap->dev, "Initialized %llu cps for FaGC+ reclaim", (dmzap->nr_internal_zones*dmz_sect2blk(dmzap->dev->zone_nr_sectors)));
		dmzap_fegc_heap_init(&dmzap->fagc_heap);
	}

	queue_delayed_work(reclaim->wq, &reclaim->work, 0);

  reclaim->dmzap = dmzap;
  dmzap->nr_reclaim_class_0 = 0;
  dmzap->nr_reclaim_class_1 = 0;
  dmzap->threshold_cb = DMZAP_START_THRESHOLD_CB;
  dmzap->reclaim->nr_free_user_zones = dmzap->nr_user_exposed_zones;
  dmzap->reclaim->p_free_user_zones = 100;
	dmzap->reclaim->nr_free_zones = dmzap->nr_internal_zones;
	dmzap->reclaim->p_free_zones = 100;
  dmzap->reclaim->atime = jiffies;
	dmzap->q_length = 0;
  return 0;

err:
  if (reclaim->kc)
    dm_kcopyd_client_destroy(reclaim->kc);
err_reclaim:
  kfree(dmzap->reclaim);

  return ret;
}

/*
 * Terminate reclaim.
 */
void dmzap_dtr_reclaim(struct dmzap_target *dmzap)
{
  int i;

  if (dmzap->victim_selection_method == DMZAP_FEGC) {
		for (i = 0; i <= dmz_sect2blk(dmzap->dev->zone_nr_sectors); i++) {
			dmzap_heap_destroy(dmzap->fegc_heaps[i]);
			kfree(dmzap->fegc_heaps[i]);
		}

		kfree(dmzap->fegc_heaps);

		dmz_dev_info(dmzap->dev,
			      "Deallocated %llu heaps for FeGC reclaim",
			      (dmz_sect2blk(dmzap->dev->zone_nr_sectors) + 1));
	}

	if (dmzap->victim_selection_method == DMZAP_FAGCPLUS) {
		vfree(dmzap->fagc_cps);
		
		dmzap_heap_destroy(&dmzap->fagc_heap);
		dmz_dev_info(dmzap->dev, "Deallocated %llu cps for FaGC+ reclaim", (dmzap->nr_internal_zones*dmz_sect2blk(dmzap->dev->zone_nr_sectors)));
	}

  cancel_delayed_work_sync(&dmzap->reclaim->work);
  destroy_workqueue(dmzap->reclaim->wq);
  dm_kcopyd_client_destroy(dmzap->reclaim->kc);
  kvfree(dmzap->reclaim->cb);
  kfree(dmzap->reclaim);
	mutex_destroy(&dmzap->reclaim_lock);
}

/*
 * Suspend reclaim.
 */
void dmzap_suspend_reclaim(struct dmzap_target *dmzap)
{
	cancel_delayed_work_sync(&dmzap->reclaim->work);
}

/*
 * Resume reclaim.
 */
void dmzap_resume_reclaim(struct dmzap_target *dmzap)
{
	queue_delayed_work(dmzap->reclaim->wq, &dmzap->reclaim->work, DMZAP_IDLE_PERIOD);
}

/*
 * BIO accounting.
 */
void dmzap_reclaim_bio_acc(struct dmzap_target *dmzap)
{
	dmzap->reclaim->atime = jiffies;
}

/*
 * Start reclaim if necessary.
 */
void dmzap_schedule_reclaim(struct dmzap_target *dmzap)
{
	if (dmzap_should_reclaim(dmzap)){
		//set_bit(DMZAP_RECLAIM_KCOPY, &dmzap->flags);
		mod_delayed_work(dmzap->reclaim->wq, &dmzap->reclaim->work, 0);
	}
}
