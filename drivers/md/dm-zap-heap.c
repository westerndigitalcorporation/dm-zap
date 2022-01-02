#include "dm-zap.h"


__always_inline unsigned long long updated_cwa(struct dmzap_zone *zone, u64 jiff){
	//return zone->cwa + (zone->nr_invalid_blocks* (jiff - zone->cwa_time));
	return zone->cps;
}

int dmzap_heap_increase_size(void *arg){
	//printk(KERN_WARNING"new size: %u\n", heap->max_size*2+1);

	struct dmzap_fegc_heap *heap = (struct dmzap_fegc_heap *)arg;
	//preempt_disable();
    void* new_data = vzalloc(sizeof(struct dmzap_fegc_heap*)* (heap->max_size*2+1));
    //preempt_enable();
    memcpy(new_data, heap->data, (heap->size+1)*sizeof(struct dmzap_fegc_heap*));
    vfree(heap->data);
    heap->data = new_data;
    heap->max_size *= 2;
    return 0;
}

int dmzap_heap_decrease_size(void* arg){
	struct dmzap_fegc_heap *heap = (struct dmzap_fegc_heap *)arg;
	//preempt_disable();
    void* new_data = vzalloc(sizeof(struct dmzap_fegc_heap*)* (heap->max_size/2+1));
    //preempt_enable();
    memcpy(new_data, heap->data, (heap->size+1)*sizeof(struct dmzap_fegc_heap*));
    vfree(heap->data);
    heap->data = new_data;
    heap->max_size /= 2;
    return 0;
}

void dmzap_fegc_heap_init(struct dmzap_fegc_heap *heap){
	heap->max_size = DEF_HEAP_SIZE;
	heap->size = 0;
	heap->data = vzalloc(sizeof(struct dmzap_fegc_heap*)* (DEF_HEAP_SIZE+1));
	//spin_lock_init(&heap->lock);
	mutex_init(&heap->lock);
}

void dmzap_heap_destroy(struct dmzap_fegc_heap *heap){
	vfree(heap->data);
}

static void heap_filter_up(struct dmzap_fegc_heap *heap, int curr_pos, u64 jiff){
	int parent;
	struct dmzap_zone *target_zone;

	target_zone = heap->data[curr_pos];

	while ( curr_pos != 1 )    /* k has a parent node */
		{ /* Parent is not the root */

			parent = curr_pos/2;

			if (updated_cwa(target_zone, jiff) > updated_cwa(heap->data[curr_pos/2], jiff))
			{ 

                heap->data[curr_pos] = heap->data[curr_pos/2];
                heap->data[curr_pos]->fegc_heap_pos = curr_pos;
                curr_pos /= 2;
			}
			else
			{
			    break;
			}

		}
    heap->data[curr_pos] = target_zone;
    heap->data[curr_pos]->fegc_heap_pos = curr_pos;
}

static void heap_filter_down(struct dmzap_fegc_heap *heap, int curr_pos, u64 jiff){

    int now, child;
    struct dmzap_zone *target = heap->data[curr_pos];

    for (now = curr_pos; now * 2 <= heap->size; now = child) {
        child = now * 2;
        if (child != heap->size && updated_cwa(heap->data[child + 1], jiff) > updated_cwa(heap->data[child], jiff)) {
            child++;
        }

        if (updated_cwa(target, jiff) < updated_cwa(heap->data[child], jiff)) {
            heap->data[now] = heap->data[child];
            heap->data[now]->fegc_heap_pos = now;
        } else /* It fits there */

        {
            break;
        }
    }
    heap->data[now] = target;
    heap->data[now]->fegc_heap_pos = now;
}

void dmzap_heap_insert(struct dmzap_fegc_heap *heap, struct dmzap_zone *zone){
    
    int i;
    unsigned int j;
    u64 jiff;
    int u;
    BUG_ON(heap == NULL);
    mutex_lock(&heap->lock);
    unsigned int prev_size = heap->size;

	// for(j=1;j < heap->size; j++){
	// 	if(j != heap->data[j]->fegc_heap_pos){
	// 		dmz_dev_info(dmzap_ptr->dev, "Insert into heap %px with size: %u, seq: %u, invalid blocks: %u, cwa: %llu", heap, heap->size, zone->seq, zone->nr_invalid_blocks, zone->cwa);
	// 		dmz_dev_info(dmzap_ptr->dev, " 22real: %u, fegc_pos: %u", j, heap->data[j]->fegc_heap_pos);
	// 		BUG();
	// 	}
	// }

	jiff = jiffies;
	//spin_lock(&heap->lock);
	
     //dmz_dev_info(dmzap_ptr->dev, "Insert into heap %px with size: %u, seq: %u, invalid blocks: %u, cwa: %llu", heap, heap->size, zone->seq, zone->nr_invalid_blocks, zone->cwa);

//     for (i=1;i<= heap->size;i++){
//         dmz_dev_debug(dmzap_ptr->dev, "[%d]: %u", i, heap->data[i]->seq);
//     }

	if (heap->size == heap->max_size){
		//kthread_run(dmzap_heap_increase_size, heap, "Heap_increase");
		dmzap_heap_increase_size(heap);
	}
	heap->size++;
	heap->data[heap->size] = zone;
	heap->data[heap->size]->fegc_heap_pos = heap->size;
	heap_filter_up(heap, heap->size, jiff);
	// for(j=1;j <= heap->size; j++){
	// 	if(j != heap->data[j]->fegc_heap_pos){
	// 		dmz_dev_info(dmzap_ptr->dev, "Insert into heap %px with size: %u, seq: %u, invalid blocks: %u, cwa: %llu", heap, heap->size, zone->seq, zone->nr_invalid_blocks, zone->cwa);
	// 		dmz_dev_info(dmzap_ptr->dev, "real: %u, fegc_pos: %u", j, heap->data[j]->fegc_heap_pos);
	// 		BUG();
	// 	}
	// }
//     for (i=1;i<= heap->size;i++){
//         dmz_dev_debug(dmzap_ptr->dev, "[%d]: %u", i, heap->data[i]->seq);
//     }

    //spin_unlock(&heap->lock);
    if (prev_size != (heap->size-1)){
	    dmz_dev_info(dmzap_ptr->dev, "prev: %u, now: %u", prev_size, heap->size);
		assert_heap_is_ok(dmzap_ptr, heap);
	    BUG();
    }
    //spin_unlock(&heap->lock);
//     for (u=1;u<=heap->size;u++){
// 	    if (heap->data[u]->fegc_heap_pos != u){
// 		dmz_dev_err(dmzap_ptr->dev, "heap: %px, zone: %px, seq: %u, u: %d, fegc_pos: %u, heap->data[u]: %px, heap size: %u", heap, zone, zone->seq, u, heap->data[u]->fegc_heap_pos, heap->data[u], heap->size);
// 	    	BUG();
// 	    }
//     }
    mutex_unlock(&heap->lock);

}

void dmzap_heap_update(struct dmzap_fegc_heap *heap, unsigned int pos){
	u64 jiff;

	//dmz_dev_info(dmzap_ptr->dev, "update in heap %px with size: %u, pos: %d, seq: %u", heap, heap->size, pos);

	jiff = jiffies;
    //if (spin_trylock(&heap->lock) == 0){
    //    BUG();
    //}
    //mdelay(1);
    //spin_lock(&heap->lock);
    BUG_ON(heap == NULL);
    mutex_lock(&heap->lock);
    //udelay(10);
    //usleep_range(10, 10);
	if (pos > heap->size){
		dmz_dev_info(dmzap_ptr->dev, "Update in heap %px with size: %u, pos: %u", heap, heap->size, pos);
		return;
	}
    BUG_ON(pos > heap->size);

	if ( pos == 1 || updated_cwa(heap->data[pos/2], jiff) > updated_cwa(heap->data[pos], jiff)){
        	heap_filter_down(heap, pos, jiff);
	}
	else {
		heap_filter_up(heap, pos, jiff);
	}
	//spin_unlock(&heap->lock);
	//dmz_dev_info(dmzap_ptr->dev, "finished update in heap %px with size: %u, pos: %d, seq: %u", heap, heap->size, pos);
	mutex_unlock(&heap->lock);
}
struct dmzap_zone* dmzap_heap_delete(struct dmzap_fegc_heap *heap, struct dmzap_zone* zone){
        
    struct dmzap_zone *ret;
    int i;
    unsigned int j;
    int u, w;
    u64 jiff;
    unsigned int pos;
    unsigned int prev_size;
    //dmz_dev_info(dmzap_ptr->dev, "Delete %px, %u", heap, pos);
    //spin_lock(&heap->lock);
    BUG_ON(zone == NULL || heap == NULL);
    mutex_lock(&heap->lock);
	//dmz_dev_err(dmzap_ptr->dev, "Delete0");
    pos = zone->fegc_heap_pos;

//     if (heap->data[zone->fegc_heap_pos] != zone){
// 	    dmz_dev_err(dmzap_ptr->dev, "Inconsistency: seq: %u, invalidas: %d", zone->seq, zone->nr_invalid_blocks);
// 	    BUG();
//     }
    prev_size = heap->size;

    jiff = jiffies;

	// if (pos > heap->size){
	// 	dmz_dev_err(dmzap_ptr->dev, "Delete from heap %px with size: %u, pos: %d, invalids: %d", heap, heap->size, pos, zone->nr_invalid_blocks);
	// 	for (w=0;w < dmzap_ptr->dev->zone_nr_blocks;w++){
	// 		for (u=1;u <= dmzap_ptr->fegc_heaps[w]->size;u++){
	// 			if (dmzap_ptr->fegc_heaps[w]->data[u] == zone)
	// 				dmz_dev_err(dmzap_ptr->dev, "FOUND in pos: %d in heap: %d", u, w);
	// 		}
	// 	}
	// }
	
	// for(j=1;j < heap->size; j++){
	// 	if(j != heap->data[j]->fegc_heap_pos){
	// 		dmz_dev_info(dmzap_ptr->dev, "Delete from heap %px with size: %u, pos: %d, seq: %u", heap, heap->size, pos, heap->data[pos]->seq);
	// 		dmz_dev_info(dmzap_ptr->dev, " 22real: %u, fegc_pos: %u", j, heap->data[j]->fegc_heap_pos);
	// 		BUG();
	// 	}
	// }

	
    BUG_ON(pos > heap->size);
    

//     for (i=1;i<= heap->size;i++){
//         dmz_dev_debug(dmzap_ptr->dev, "[%d]: %u", i, heap->data[i]->seq);
//     }
	//dmz_dev_err(dmzap_ptr->dev, "Delete1");
    ret = heap->data[pos];
    if (pos == heap->size){
	    ret->fegc_heap_pos = -1;
	    heap->size--;
	    //spin_unlock(&heap->lock);
	    mutex_unlock(&heap->lock);
	    return ret;
    }
    
    heap->data[pos] = heap->data[heap->size--];
    heap->data[pos]->fegc_heap_pos = pos;
	
    if ( pos == 1 || updated_cwa(heap->data[pos/2], jiff) > updated_cwa(heap->data[pos], jiff)){
        heap_filter_down(heap, pos, jiff);
    }
    else {
        heap_filter_up(heap, pos, jiff);
    }

    ret->fegc_heap_pos = -1;

//     for (i=1;i<= heap->size;i++){
//         dmz_dev_debug(dmzap_ptr->dev, "[%d]: %u", i, heap->data[i]->seq);
//     }
    
    if (heap->size < (heap->max_size/2 - heap->max_size/10))
	dmzap_heap_decrease_size(heap);
// 	kthread_run(dmzap_heap_decrease_size, heap, "Heap_decrease");

	// for(j=1;j <= heap->size; j++){
	// 	if(j != heap->data[j]->fegc_heap_pos){
	// 		dmz_dev_info(dmzap_ptr->dev, "Delete from heap %px with size: %u, pos: %d, seq: %u", heap, heap->size, pos, heap->data[pos]->seq);
	// 		dmz_dev_info(dmzap_ptr->dev, "real: %u, fegc_pos: %u", j, heap->data[j]->fegc_heap_pos);
	// 		BUG();
	// 	}
	// }
	//dmz_dev_err(dmzap_ptr->dev, "Delete2");
	if (prev_size != (heap->size+1)){
	    dmz_dev_info(dmzap_ptr->dev, "prev: %u, now: %u", prev_size, heap->size);
	    BUG();
    }
    //spin_unlock(&heap->lock);
//     for (u=1;u<=heap->size;u++){
// 	    if (heap->data[u]->fegc_heap_pos != u)
// 	    	BUG();
//     }
    mutex_unlock(&heap->lock);
    return ret;
}

// void dmzap_heap_insert(struct dmzap_fegc_heap *heap, struct dmzap_zone *zone){
// 	if (heap->size == heap->max_size){
// 		dmzap_heap_increase_size(heap);
// 	}
// 	heap->size++;
// 	heap->data[heap->size] = zone;
// 	unsigned int now = heap->size;
// 	while (heap->data[now / 2] < zone) {
// 		heap->data[now] = heap->data[now / 2];
// 		now /= 2;
// 		}

// 	heap->data[now] = zone; 
// }

// struct dmzap_zone* dmzap_heap_delete_min(struct dmzap_fegc_heap *heap){

//     unsigned int now, child;
//     struct dmzap_zone *min_element, *last_element;
//     min_element = heap->data[1];
//     last_element = heap->data[heap->size--];

//     for (now = 1; now * 2 <= heap->size; now = child) {

//         /* child is the index of the element which is minimum among both the children */
//         /* Indexes of children are i*2 and i*2 + 1*/
//         child = now * 2;

//         /*child!=heapSize beacuse heap[heapSize+1] does not exist, which means it has only one
//          child */

//         if (child != heap->size && heap->data[child + 1] > heap->data[child]) {
//             child++;
//         }

//         /* To check if the last element fits ot not it suffices to check if the last element
//          is less than the minimum element among both the children*/

//         if (last_element > heap->data[child]) {
//             heap->data[now] = heap->data[child];
//         } else /* It fits there */

//         {
//             break;
//         }
//     }

//     heap->data[now] = last_element;
//     return last_element;
// }

void assert_heap_ok(struct dmzap_target *dmzap, int num){
    int i, j;

    char *numbers = kzalloc(sizeof(char)*dmzap->nr_internal_zones, GFP_KERNEL);

    for (i=0; i <= dmz_sect2blk(dmzap->dev->zone_nr_sectors);i++){
        for (j=1; j <= dmzap->fegc_heaps[i]->size; j++){
            if (numbers[dmzap->fegc_heaps[i]->data[j]->seq]){
                dmz_dev_info(dmzap->dev, "Error in: %d", num);
                BUG();
            }
            numbers[dmzap->fegc_heaps[i]->data[j]->seq] = 1;
        }
    }
    kfree(numbers);
}

void assert_heap_is_ok(struct dmzap_target *dmzap, struct dmzap_fegc_heap *heap){
    int i;

    char *numbers = kzalloc(sizeof(char)*dmzap->nr_internal_zones, GFP_KERNEL);

    for (i=1; i <= heap->size;i++){
            if (numbers[heap->data[i]->seq]){
                dmz_dev_info(dmzap->dev, "Error in: %px", heap);
                BUG();
            }
            numbers[heap->data[i]->seq] = 1;
    }
    kfree(numbers);
}

bool heaps_are_ok(struct dmzap_target *dmzap){
    int i, j;

    char *numbers = kzalloc(sizeof(char)*dmzap->nr_internal_zones, GFP_KERNEL);

    for (i=0; i <= dmz_sect2blk(dmzap->dev->zone_nr_sectors);i++){
        for (j=1; j <= dmzap->fegc_heaps[i]->size; j++){
            if (numbers[dmzap->fegc_heaps[i]->data[j]->seq]){
		dmz_dev_info(dmzap_ptr->dev, "Error in %u in heap: %d", dmzap->fegc_heaps[i]->data[j]->seq, i);
		kfree(numbers);
                return false;
            }
            numbers[dmzap->fegc_heaps[i]->data[j]->seq] = 1;
        }
    }
    kfree(numbers);
    return true;
}

void heap_print(struct dmzap_target *dmzap, int heap_num){
    int i, j;
	dmz_dev_info(dmzap_ptr->dev, "Heap %d", heap_num);
	for (j=1; j <= dmzap->fegc_heaps[heap_num]->size; j++){
		dmz_dev_info(dmzap_ptr->dev, "[%d]: %u", j, dmzap->fegc_heaps[heap_num]->data[j]->seq);
	}
}

