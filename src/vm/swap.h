#ifndef SWAP_H
#define SWAP_H

/** The pointer to the swap block device. */
struct block* global_swap_block;

void read_from_block(void* frame, int index);
void write_from_block(void* frame, int index);

#endif /**< vm/swap.h */
