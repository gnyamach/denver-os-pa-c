/*
 * Created by Ivo Georgiev on 2/9/16.
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
node_pt const add_node(pool_mgr_pt pMgr);

void add_gap(pool_mgr_pt pMgr, const node_pt pNode);

//TESTED
alloc_status mem_init() {
    // ensure that it's called only once until mem_free
    if (pool_store){
        printf("Pool Store already Allocated.\r\n");
        return ALLOC_CALLED_AGAIN;
    }
    // allocate the pool store with initial capacity.
    // An array if MEM_POOL_STORE_INIT_CAPACITY is created.
    // note: holds pointers only, other functions to allocate/deallocate
    pool_store = (pool_mgr_pt *) calloc(MEM_POOL_STORE_INIT_CAPACITY, sizeof(pool_mgr_t));
    //Have to check that allocation succeeded to eliminate errors
    if(pool_store == NULL) {
        printf("calloc was unsuccessful for pool_store\r\n");
    }
    //Initialization of other variables in pool_store
    pool_store_size = 0;
    pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
    /*
        int i;
        for(i =0; i< MEM_POOL_STORE_INIT_CAPACITY; i++){
            printf("size of [%d] is: %d \n\r",i, pool_store[i] ->used_nodes);
            pool_store++;
        }
     */
    assert(pool_store);
    printf("pool_store initialization successful.\r\n");
    return ALLOC_OK;
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
    for(unsigned int i = 0;i < pool_store_size; i++ ){
         mem_pool_close(&pool_store[i]-> pool);
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
    // make sure there the pool store is allocated
    if (pool_store == NULL) {
        //initialize memory if not initialized
        if (mem_init() != ALLOC_OK) {
            // expand the pool store, if necessary and return NULL on failure
            if (_mem_resize_pool_store() != ALLOC_OK) {
                return NULL;
            }
        }
    }

    //confirm that the pool store exists
    assert(pool_store);
    // allocate a new mem pool mgr
    pool_mgr_pt temp_pool_mgr = (pool_mgr_pt) calloc(size, sizeof(pool_mgr_t));
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
    temp_pool_mgr->gap_ix = temp_ix;
    temp_pool_mgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;
    temp_pool_mgr->node_heap = temp_heap;
    temp_pool_mgr->total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;
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

    // check if this pool is allocated
    // check if pool has only one gap
    // check if it has zero allocations
    if (pool->mem == NULL && pool->num_gaps == 1 && close_pool_mgr->used_nodes == 1){
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
    }
    assert(pool_store);
    return ALLOC_OK;
}

/*
 * This function performs a single allocation of  size in bytes from the given memory pool. Allocations from different
 * memory pools are independent.
 */
alloc_pt mem_new_alloc(pool_pt pool, size_t size) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // check if any gaps, return null if none
    // expand heap node, if necessary, quit on error
    // check used nodes fewer than total nodes, quit on error
    // get a node for allocation:
    // if FIRST_FIT, then find the first sufficient node in the node heap
    // if BEST_FIT, then find the first sufficient node in the gap index
    // check if node found
    // update metadata (num_allocs, alloc_size)
    // calculate the size of the remaining gap, if any
    // remove node from gap index
    // convert gap_node to an allocation node of given size
    // adjust node heap:
    //   if remaining gap, need a new node
    //   find an unused one in the node heap
    //   make sure one was found
    //   initialize it to a gap node
    //   update metadata (used_nodes)
    //   update linked list (new node right after the node for allocation)
    //   add to gap index
    //   check if successful
    // return allocation record by casting the node to (alloc_pt)

    return NULL;
}
/*
 * This function deallocates the given allocation from the given memory pool.
 */
alloc_status mem_del_alloc(pool_pt pool, alloc_pt alloc) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // get node from alloc by casting the pointer to (node_pt)
    // find the node in the node heap
    // this is node-to-delete
    // make sure it's found
    // convert to gap node
    // update metadata (num_allocs, alloc_size)
    // if the next node in the list is also a gap, merge into node-to-delete
    //   remove the next node from gap index
    //   check success
    //   add the size to the node-to-delete
    //   update node as unused
    //   update metadata (used nodes)
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

    // this merged node-to-delete might need to be added to the gap index
    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    //   remove the previous node from gap index
    //   check success
    //   add the size of node-to-delete to the previous
    //   update node-to-delete as unused
    //   update metadata (used_nodes)
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
    //   change the node to add to the previous node!
    // add the resulting node to the gap index
    // check success

    return ALLOC_FAIL;
}

/*
 * This function returns a new dynamically allocated array of the pool  segments  (allocations or gaps) in the order
 * in which they are in the pool. The number of segments is returned in  num_segments . The caller is responsible
 * for freeing the array.
 */
// NOTE: Allocates a dynamic array. Caller responsible for releasing.
void mem_inspect_pool(pool_pt pool, pool_segment_pt *segments, unsigned *num_segments) {
    // get the mgr from the pool
    // allocate the segments array with size == used_nodes
    // check successful
    // loop through the node heap and the segments array
    //    for each node, write the size and allocated in the segment
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
    /* check if necessary
    /*
                if (((float) pool_store_size / pool_store_capacity)
                    > MEM_POOL_STORE_FILL_FACTOR) {...}
     */
    // don't forget to update capacity variables

    return ALLOC_FAIL;
}

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    // see above

    return ALLOC_FAIL;
}

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    // see above

    return ALLOC_FAIL;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {

    // expand the gap index, if necessary (call the function)
    // add the entry at the end
    // update metadata (num_gaps)
    // sort the gap index (call the function)
    // check success

    return ALLOC_FAIL;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {
    // find the position of the node in the gap index
    // loop from there to the end of the array:
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    // update metadata (num_gaps)
    // zero out the element at position num_gaps!

    return ALLOC_FAIL;
}

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    //    if the size of the current entry is less than the previous (u - 1)
    //       swap them (by copying) (remember to use a temporary variable)

    return ALLOC_FAIL;
}


// Test print
void print_pool2(pool_pt pool) {

    // Upcast
    const pool_mgr_pt pool_mgr = (pool_mgr_pt)pool;

    printf("P\t%d\t%d\t%d\t%d\r\n--------------\r\n", (int) pool_mgr->pool.alloc_size, (int) pool_mgr->pool.total_size,
                                pool_mgr->pool.num_allocs, pool_mgr->pool.num_gaps);
    unsigned j;
    for ( j= 0; j < pool_mgr->used_nodes; ++j) {

        printf("N%d:\t%p\t%d\t", j, (void *) pool_mgr->node_heap[j].alloc_record.mem, (int) pool_mgr->node_heap[j].alloc_record.size);

        if(pool_mgr->node_heap[j].allocated) {
            printf("\tAllocated");
        } else {
            printf("\t\t");
        }

        if(pool_mgr->node_heap[j].used) {
            printf("\tUsed");
        } else {
            printf("\t");
        }

        printf("\r\n");

    }

    printf("\r\n");
    unsigned  i;
    for ( i = 0; i < pool_mgr->pool.num_gaps; ++i) {
        printf("G%d:\t%p\t%d\r\n", i, (void *) pool_mgr->gap_ix[i].node->alloc_record.mem, (int) pool_mgr->gap_ix[i].size);
    }

    printf("\r\n");

}
