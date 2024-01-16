#include "swap.h"
#include "devices/block.h"

void
init_swap ()
{
  global_swap_block = block_get_role(BLOCK_SWAP);
}

/** Read into page size of blocks. (8)

    frame is the physical frame I want to read into.
    index is the starting index of the block that is free. */
void
read_from_block(void* frame, int index)
{
  for (int i = 0; i < 8; i++)
    {
      /* Each read will read 512 bytes, therefore we need to read
         8 times, each at 512 increments of the frame. */
      block_read (global_swap_block, index + i, frame + (i * BLOCK_SECTOR_SIZE));
    }
}

/** Write from page size of blocks. (8)

    frame is the physical frame I want to write from.
    index is the starting index of the block that is free. */
void
write_from_block(void* frame, int index)
{
  for (int i = 0; i < 8; i++)
    {
      /* Each write will write 512 bytes, therefore we need to write
         8 times, each at 512 increments of the frame. */
      block_write (global_swap_block, index + i, frame + (i * BLOCK_SECTOR_SIZE));
    }
}