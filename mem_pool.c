/*
 * Created by Ivo Georgiev on 2/9/16.
 * Edit by Gideon Nyamachere
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h> // for perror()
#include "mem_pool.h"
#include <stdbool.h>

/*************/
/*           */
/* Constants */
/*           */
/*************/
static const float      MEM_FILL_FACTOR                 = 0.75;
static const unsigned   MEM_EXPAND_FACTOR               = 2;

static const unsigned   MEM_POOL_STORE_INIT_CAPACITY    = 20;
static const float      MEM_POOL_STORE_FILL_FACTOR      = 0.75;
static const unsigned   MEM_POOL_STORE_EXPAND_FACTOR    = 2;

static const unsigned   MEM_NODE_HEAP_INIT_CAPACITY     = 40;
static const float      MEM_NODE_HEAP_FILL_FACTOR       = 0.75;
static const unsigned   MEM_NODE_HEAP_EXPAND_FACTOR     = 2;

static const unsigned   MEM_GAP_IX_INIT_CAPACITY        = 40;
static const float      MEM_GAP_IX_FILL_FACTOR          = 0.75;
static const unsigned   MEM_GAP_IX_EXPAND_FACTOR        = 2;




/*********************/
/*                   */
/* Type declarations */
/*                   */
/*********************/
typedef struct _node {
    alloc_t alloc_record;
    unsigned used;
    unsigned allocated;
    struct _node *next, *prev; // doubly-linked list for gap deletion
} node_t, *node_pt;

typedef struct _gap {
    size_t size;
    node_pt node;
} gap_t, *gap_pt;

typedef struct _pool_mgr {
    pool_t pool;
    node_pt node_heap;
    unsigned int total_nodes;
    unsigned used_nodes;
    gap_pt gap_ix;
    unsigned gap_ix_capacity;
} pool_mgr_t, *pool_mgr_pt;


/***************************/
/*                         */
/* Static global variables */
/*                         */
/***************************/
static pool_mgr_pt *pool_store = NULL; // an array of pointers, only expand
static unsigned pool_store_size = 0;
static unsigned pool_store_capacity = 0;

/********************************************/
/*                                          */
/* Forward declarations of static functions */
/*                                          */
/********************************************/
static alloc_status _mem_resize_pool_store();
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr);
static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status
        _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                           size_t size,
                           node_pt node);
static alloc_status
        _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                size_t size,
                                node_pt node);
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr);

/*                                      */
/* Definitions of user-facing functions */
/*                                      */
/*
 * This function should be called first and called only once until a corresponding  mem_free() . It initializes the
 * memory pool (manager) store, a data structure which stores records for separate memory pools.
 */
alloc_status mem_init() {
    // ensure that it's called only once until mem_free
    if (pool_store != NULL){
        //printf("Pool Store already Allocated.\r\n");
        return ALLOC_CALLED_AGAIN;
    }
    // allocate the pool store with initial capacity.
    // An array if MEM_POOL_STORE_INIT_CAPACITY is created.
    // note: holds pointers only, other functions to allocate/deallocate
    pool_mgr_pt* temp_pool_store = (pool_mgr_pt*) calloc(MEM_POOL_STORE_INIT_CAPACITY, sizeof(pool_mgr_t));
    assert(temp_pool_store);
    if (temp_pool_store != NULL) {
        //Initialization of other variables in pool_store
        pool_store_size = 0;
        pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
        pool_store = temp_pool_store;
        printf("pool_store initialization successful.\r\n");
        return ALLOC_OK;
    }
}


/*
 * This function should be called last and called only once for each corresponding  mem_init() . It frees the pool
 * (manager) store memory.
 */

alloc_status mem_free() {
    // ensure that it's called only once for each mem_init
    if (pool_store == NULL){
        return ALLOC_CALLED_AGAIN;
    }

    // make sure all pool managers have been deallocated
    for(unsigned int i = 0;i < pool_store_capacity; i++ ){
        if (pool_store[i] != NULL) {
            return ALLOC_CALLED_AGAIN;
        }
    }
    // can free the pool store array
    // update static variables
    free(pool_store);
    pool_store = NULL;
    pool_store_size = 0;
    pool_store_capacity = 0;

    return ALLOC_OK;
}

/*
 * This function allocates a single memory pool from which separate allocations can be performed. It takes a  size  in
 * bytes, and an allocation policy, either  FIRST_FIT  or  BEST_FIT .
 * */
pool_pt mem_pool_open(size_t size, alloc_policy policy) {
    // make sure that the pool store is allocated
    assert(pool_store);

    // expand the pool store, if necessary
    _mem_resize_pool_store();

    // allocate a new mem pool mgr
    pool_mgr_pt temp_pool_mgr = (pool_mgr_pt) calloc(1, sizeof(pool_mgr_t));
    // check success, on error return null
    if (temp_pool_mgr == NULL) {
        printf("pool_mgr initialization NOT successful.\r\n");
        return NULL;
    }

    // allocate a new memory pool
    char* temp_mem_pool = (char *) calloc(size, sizeof(char));
    // check success, on error deallocate mgr and return null
    if (temp_mem_pool == NULL) {
        printf("pool.mem initialization NOT successful.\r\n");
        mem_free(temp_pool_mgr);
        return NULL;
    }

    // allocate a new node heap
    node_pt temp_heap = (node_pt) calloc(MEM_NODE_HEAP_INIT_CAPACITY, sizeof(node_t));
    // check success, on error deallocate mgr/pool and return null
    if(temp_heap == NULL){
        free(temp_pool_mgr);
        free(temp_mem_pool);
        return NULL;
    }

    // allocate a new gap index
    gap_pt temp_ix = (gap_pt)calloc(MEM_GAP_IX_INIT_CAPACITY, sizeof(gap_t));
    // check success, on error deallocate mgr/pool/heap and return null
    if (temp_ix == NULL){
        free(temp_pool_mgr);
        free(temp_mem_pool);
        free(temp_heap);
        return NULL;
    }

    // assign all the pointers and update meta data:
    temp_pool_mgr->pool.mem = temp_mem_pool;
    temp_pool_mgr->pool.alloc_size = 0;
    temp_pool_mgr->pool.num_allocs = 0;
    temp_pool_mgr->pool.num_gaps = 1;
    temp_pool_mgr->pool.total_size = size;
    temp_pool_mgr->pool.policy = policy;

    //   initialize top node of node heap
    temp_heap[0].alloc_record.mem = temp_mem_pool;
    temp_heap[0].alloc_record.size = size;
    temp_heap[0].used = 1;
    temp_heap[0].allocated = 0;
    temp_heap[0].prev = NULL;
    temp_heap[0].next = NULL;

    //   initialize top node of gap index
    temp_ix[0].size = size;
    temp_ix[0].node = temp_heap;

    //   initialize pool mgr
    temp_pool_mgr->node_heap = temp_heap;
    temp_pool_mgr->total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;
    temp_pool_mgr->gap_ix = temp_ix;
    temp_pool_mgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;
    temp_pool_mgr->used_nodes = 1;

    //   link pool mgr to pool store
    pool_store[pool_store_size] = temp_pool_mgr;
    pool_store_size ++;
    printf("The pool_store_size: %d \r\n", pool_store_size);

    // return the address of the mgr, cast to (pool_pt)
    return (pool_pt)temp_pool_mgr;

}

/*
 * This function deallocates a single memory pool.
 */
alloc_status mem_pool_close(pool_pt pool) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt close_pool_mgr = (pool_mgr_pt)pool;
    if (!(close_pool_mgr)){
        return ALLOC_FAIL;
    }

    // check if this pool is allocated
    // check if pool has only one gap
    // check if it has zero allocations
    if ((pool->mem != NULL )&& (pool->num_gaps == 1) && (close_pool_mgr->used_nodes == 1)){
        // free memory pool
        // free node heap
        // free gap index
        free(pool->mem);
        free(close_pool_mgr->node_heap);
        free(close_pool_mgr->gap_ix);
        // find mgr in pool store and set to null
        for(int i = 0; i < pool_store_size; i++){
            if(pool_store[i]== close_pool_mgr){
                pool_store[i] = NULL;
                break;
            }
        }
        // note: don't decrement pool_store_size, because it only grows
        // free mgr
        free(close_pool_mgr);
        return ALLOC_OK;
    }
    assert(pool_store);
    return ALLOC_NOT_FREED;
}

/*
 * This function performs a single allocation of  size in bytes from the given memory pool. Allocations from different
 * memory pools are independent.
 */
alloc_pt mem_new_alloc(pool_pt pool, size_t size) {
    // printf("This part did not run, pool size is: %d", size);
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt temp_pool_mgr = (pool_mgr_pt)pool;
    // check if any gaps, return null if none
    if (pool->num_gaps == 0){
        printf("The mem_new_alloc FAILED ie found gaps");
        return NULL;
    }
    // expand heap node, if necessary, quit on error
    _mem_resize_node_heap(temp_pool_mgr);
    // check used nodes fewer than total nodes, quit on error
    if(temp_pool_mgr->used_nodes > temp_pool_mgr->total_nodes){
        printf("ERROR: The mem_new_alloc used nodes < than total nodes found");
        return NULL;
    }
    // get a node for allocation:
    node_pt fit_node = NULL;

    // if FIRST_FIT, then find the first sufficient node in the node heap
    if(pool->policy == FIRST_FIT){
        for(unsigned int i = 0; i < temp_pool_mgr->total_nodes; i++){
            if((temp_pool_mgr->node_heap[i].allocated == 0)&&(temp_pool_mgr->node_heap[i].alloc_record.size >= size)){
                fit_node = &temp_pool_mgr->node_heap[i];
                break;
            }
        }
    }

    // if BEST_FIT, then find the first sufficient node in the gap index
    if(pool->policy == BEST_FIT){
        for(unsigned int i = 0; i < temp_pool_mgr->pool.num_gaps; i++){
            if(temp_pool_mgr->gap_ix[i].size >= size){
                fit_node = temp_pool_mgr->gap_ix[i].node;
                break;
            }
        }
    }
    // check if node found
    if(fit_node == NULL){
        return NULL;
    }
    // update metadata (num_allocs, alloc_size)
    temp_pool_mgr->pool.num_allocs++;
    temp_pool_mgr->pool.alloc_size += size;
    // calculate the size of the remaining gap, if any
    size_t re_size = fit_node->alloc_record.size - size;
    // remove node from gap index
    _mem_remove_from_gap_ix(temp_pool_mgr, fit_node->alloc_record.size, fit_node);
    // convert gap_node to an allocation node of given size
    fit_node->allocated = 1;
    fit_node->alloc_record.size = size;
    // adjust node heap:
    if(re_size > 0){
        //   if remaining gap, need a new node
        node_pt unused_node = NULL;
        //   find an unused one in the node heap
        for(unsigned i = 0; i < temp_pool_mgr->total_nodes; i++){
            if(temp_pool_mgr->node_heap[i].used == 0){
                unused_node = &temp_pool_mgr->node_heap[i];
                break;
            }
        }
        //   make sure one was found
        assert(unused_node);
        //   initialize it to a gap node
        unused_node->alloc_record.size = re_size;
        unused_node->allocated = 0;
        unused_node->used = 1;
        unused_node->alloc_record.mem =size* sizeof(char) + fit_node->alloc_record.mem;
        //   update metadata (used_nodes)
        temp_pool_mgr->used_nodes ++;
        //   update linked list (new node right after the node for allocation)
        unused_node->next = fit_node->next;
        if((fit_node->next) && (fit_node->next != NULL)){
            fit_node->next->prev = unused_node;
        }
        fit_node->next = unused_node;
        unused_node->prev = fit_node;

        //   add to gap index
        alloc_status status;
        status = _mem_add_to_gap_ix(temp_pool_mgr, unused_node->alloc_record.size, unused_node);
        //   check if successful
        assert(status == ALLOC_OK);
    }

    // return allocation record by casting the node to (alloc_pt)
    return (alloc_pt)fit_node;
}

/*
 * This function deallocates the given allocation from the given memory pool.
 */
alloc_status mem_del_alloc(pool_pt pool, alloc_pt alloc) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt temp_pool_mgr = (pool_mgr_pt)pool;
    // get node from alloc by casting the pointer to (node_pt)
    node_pt temp_node = (node_pt)alloc;
    // find the node in the node heap
    node_pt node_to_del = NULL;
    for (unsigned i = 0; i < temp_pool_mgr->total_nodes; i++){
        if(temp_node == &temp_pool_mgr->node_heap[i]){
            // this is node-to-delete
            node_to_del = &temp_pool_mgr->node_heap[i];
            break;
        }
    }
    // make sure it's found
    if (node_to_del == NULL){
        return ALLOC_FAIL;
    };

    // convert to gap node
    node_to_del->allocated = 0;
    // update metadata (num_allocs, alloc_size)
    temp_pool_mgr->pool.alloc_size -= node_to_del->alloc_record.size;
    temp_pool_mgr->pool.num_allocs --;
    // if the next node in the list is also a gap, merge into node-to-delete
    node_pt node_to_add = NULL;
    if((node_to_del->next) && (node_to_del->next->allocated == 0)){
        //   remove the next node from gap index
        node_pt next_node = node_to_del->next;
        size_t node_size = next_node->alloc_record.size;
        alloc_status status = _mem_remove_from_gap_ix(temp_pool_mgr, node_size, next_node);
        //   check success
        assert(status == ALLOC_OK);

        //   add the size to the node-to-delete
        node_to_del->alloc_record.size += node_size;
        //   update node as unused
        next_node->used = 0;
        //   update metadata (used nodes)
        temp_pool_mgr->used_nodes --;
        //   update linked list:
        /*
                        if (next->next) {
                            next->next->prev = node_to_del;
                            node_to_del->next = next->next;
                        } else {
                            node_to_del->next = NULL;
                        }
                        next->next = NULL;
                        next->prev = NULL;
         */

        if (next_node->next){
            next_node->next->prev = node_to_del;
            node_to_del->next =next_node->next;
        }else{
            node_to_del->next =NULL;
        }
        next_node->next = NULL;
        next_node->prev = NULL;
        //used down below
        // TODO: this merged node-to-delete might need to be added to the gap index
        node_to_add = node_to_del;
    }


    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    if((node_to_del->prev)&&(node_to_del->prev->allocated == 0)){
        //   remove the previous node from gap index
        node_pt prev_node = node_to_del->prev;
        size_t prev_node_size = node_to_del->prev->alloc_record.size;
        alloc_status status = _mem_remove_from_gap_ix(temp_pool_mgr ,prev_node_size, prev_node);
        //   check success
        assert(status == ALLOC_OK);

        //   add the size of node-to-delete to the previous
        prev_node->alloc_record.size += node_to_del->alloc_record.size;
        //   update node-to-delete as unused
        node_to_del->used = 0;
        //   update metadata (used_nodes)
        temp_pool_mgr->used_nodes --;
        //   update linked list
        /*
                    if (node_to_del->next) {
                        prev->next = node_to_del->next;
                        node_to_del->next->prev = prev;
                    } else {
                        prev->next = NULL;
                    }
                    node_to_del->next = NULL;
                    node_to_del->prev = NULL;
        */
        if(node_to_del->next){
            prev_node->next = node_to_del->next;
            node_to_del->next->prev = prev_node;
        }else{
            prev_node->next = NULL;
        }
        node_to_del->next = NULL;
        node_to_del->prev =NULL;
        //   change the node to add to the previous node!
        node_to_add = prev_node;
    }
    // add the resulting node to the gap index
    if(node_to_add){
        size_t size_to_add = node_to_add->alloc_record.size;
        alloc_status status = (_mem_add_to_gap_ix(temp_pool_mgr, size_to_add, node_to_add));
        // check success
        assert( status == ALLOC_OK);

    }else{
        size_t size_to_del = node_to_del->alloc_record.size;
        alloc_status status = (_mem_add_to_gap_ix(temp_pool_mgr, size_to_del, node_to_del));
        // check success
        assert( status == ALLOC_OK);

    }
    return ALLOC_OK;
}


void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments) {
    // get the mgr from the pool
    pool_mgr_pt mgr = (pool_mgr_pt) pool;
    // allocate the segments array with size == used_nodes
    pool_segment_pt segs = (pool_segment_pt) calloc(mgr->used_nodes, sizeof(pool_segment_t));
    // check successful
    assert(segs);
    // loop through the node heap and the segments array
    //    for each node, write the size and allocated in the segment
    node_pt first_node = mgr->node_heap;
    int i = 0;
    while (first_node) {
        segs[i].size = first_node->alloc_record.size;
        segs[i].allocated = first_node->allocated;
        first_node = first_node->next;
        i++;
    }

    *segments = segs;
    *num_segments = mgr->used_nodes;
    // "return" the values:
    /*
                    *segments = segs;
                    *num_segments = pool_mgr->used_nodes;
     */
}



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/
static alloc_status _mem_resize_pool_store() {
    // check if necessary
    /*
                if (((float) pool_store_size / pool_store_capacity)
                    > MEM_POOL_STORE_FILL_FACTOR) {...}
     */
    // don't forget to update capacity variables
    if (((float) pool_store_size / pool_store_capacity) > MEM_POOL_STORE_FILL_FACTOR) {
        pool_mgr_pt* resize = (pool_mgr_pt*) realloc(pool_store, pool_store_capacity*MEM_POOL_STORE_EXPAND_FACTOR );
        assert(resize);
        pool_store = resize;
        pool_store_capacity = pool_store_capacity*MEM_POOL_STORE_EXPAND_FACTOR;
        return ALLOC_OK;
    }

    return ALLOC_FAIL;
}

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    if (((float) pool_mgr->used_nodes / pool_mgr->total_nodes) > MEM_NODE_HEAP_FILL_FACTOR) {
        node_pt resize = realloc(pool_mgr->node_heap, pool_mgr->total_nodes*MEM_NODE_HEAP_EXPAND_FACTOR);
        assert(resize);
        pool_mgr->node_heap = resize;
        pool_mgr->total_nodes = pool_mgr->total_nodes*MEM_NODE_HEAP_EXPAND_FACTOR;
        return ALLOC_OK;
    }

    return ALLOC_FAIL;
}

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    if (((float) pool_mgr->pool.num_gaps / pool_mgr->gap_ix_capacity) > MEM_GAP_IX_FILL_FACTOR) {
        gap_pt resize = realloc(pool_mgr->gap_ix, pool_mgr->gap_ix_capacity*MEM_GAP_IX_EXPAND_FACTOR);
        assert(resize);
        pool_mgr->gap_ix = resize;
        pool_mgr->gap_ix_capacity = pool_mgr->gap_ix_capacity*MEM_GAP_IX_EXPAND_FACTOR;
        return ALLOC_OK;
    }

    return ALLOC_FAIL;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {

    // expand the gap index, if necessary (call the function)
    _mem_resize_gap_ix(pool_mgr);
    // add the entry at the end
    gap_t new_gap;
    new_gap.size = size;
    new_gap.node = node;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps] = new_gap;
    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps += 1;
    // sort the gap index (call the function)
    assert(_mem_sort_gap_ix(pool_mgr) == ALLOC_OK);
    // check success

    return ALLOC_OK;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {
    // find the position of the node in the gap index
    int index_pos = 0;
    for (int i = 0; i < pool_mgr->pool.num_gaps; i++) {
        if ((pool_mgr->gap_ix[i].size == size) && (pool_mgr->gap_ix[i].node == node)) {
            index_pos = i;
            break;
        }
    }
    // loop from there to the end of the array:
    for (int i = index_pos; i < pool_mgr->pool.num_gaps - 1; i++) {
        pool_mgr->gap_ix[i] = pool_mgr->gap_ix[i+1];
    }
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps -= 1;
    // zero out the element at position num_gaps!
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = 0;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = NULL;

    return ALLOC_OK;
}

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    for (int i = pool_mgr->pool.num_gaps -1; i > 0; i--) {
        if (pool_mgr->gap_ix[i].size < pool_mgr->gap_ix[i-1].size) {
            gap_t temp = pool_mgr->gap_ix[i];
            pool_mgr->gap_ix[i] = pool_mgr->gap_ix[i-1];
            pool_mgr->gap_ix[i-1] = temp;
        }
        else if (pool_mgr->gap_ix[i].size == pool_mgr->gap_ix[i-1].size) {
            if (pool_mgr->gap_ix[i].node->alloc_record.mem < pool_mgr->gap_ix[i - 1].node->alloc_record.mem) {
                gap_t temp = pool_mgr->gap_ix[i];
                pool_mgr->gap_ix[i] = pool_mgr->gap_ix[i-1];
                pool_mgr->gap_ix[i-1] = temp;
            }
        }
    }
    //    if the size of the current entry is less than the previous (u - 1)
    //    or if the sizes are the same but the current entry points to a
    //    node with a lower address of pool allocation address (mem)
    //       swap them (by copying) (remember to use a temporary variable)

    return ALLOC_OK;
}