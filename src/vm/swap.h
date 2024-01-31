#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>

/** The pointer to the swap block device. */
struct block* global_swap_block;

/** Define the swap_index's type. */
typedef uint32_t swap_index_t;

void vm_swap_init ();

void vm_swap_read_from_block (void* kpage, swap_index_t index);
swap_index_t vm_swap_write_to_block (void* kpage);

void vm_swap_free (swap_index_t index);

#endif /**< vm/swap.h */
