#include <bitmap.h>
#include "swap.h"
#include "threads/vaddr.h"
#include "devices/block.h"

/** The number of sectors to fill a page. Default: 4096 / 512 = 8. */
static const uint32_t SECTORS_PER_PAGE = ((uint32_t) (PGSIZE / BLOCK_SECTOR_SIZE));

/** The maximum number of swapped pages stored in swap block. Default: 8192 / 8 = 1024. */
static uint32_t MAX_SWAP_PAGES_NUM;

/** The swap bitmap, to track the availability of empty memory. */
static struct bitmap *swap_bitmap;

/** Initialize global_swap_block. */
void
vm_swap_init ()
{
  ASSERT (SECTORS_PER_PAGE > 0);

  global_swap_block = block_get_role (BLOCK_SWAP);
  if (global_swap_block == NULL)
    {
      PANIC ("Error: Can't initialize global swap block");
      NOT_REACHED ();
    }

  MAX_SWAP_PAGES_NUM = (uint32_t) (block_size (global_swap_block) / SECTORS_PER_PAGE);
  swap_bitmap = bitmap_create (MAX_SWAP_PAGES_NUM);
  if (swap_bitmap == NULL)
    {
      PANIC ("Error: Can't initialize swap bitmap");
      NOT_REACHED ();
    }
  bitmap_set_all (swap_bitmap, true);
}

/** Read into page size of blocks.
    Each page needs SECTORS_PER_PAGE sectors from swap_block.

    kpage is the kernel page read into.
    index is the starting index of the block that is free. */
void
vm_swap_read_from_block (void* kpage, swap_index_t index)
{
  ASSERT (index < MAX_SWAP_PAGES_NUM);
  ASSERT (is_kernel_vaddr (kpage));

  /* Testing if the index in swap_bitmap has been occupied. */
  if (bitmap_test (swap_bitmap, index))
    PANIC ("Error, invalid read access to unassigned swap block");

  /* Each read will read 512 bytes, therefore we need to read
         SECTORS_PER_PAGE times, each at 512 increments of the page. */
  for (int i = 0; i < SECTORS_PER_PAGE; i++)
    block_read (global_swap_block,
                index * SECTORS_PER_PAGE + i,
                kpage + (i * BLOCK_SECTOR_SIZE));

  bitmap_set (swap_bitmap, index, true);
}

/** Write into page size of blocks.
    Returns the swap index.

    kpage is the physical frame I want to write from.
    index is the starting index of the block that is free. */
swap_index_t
vm_swap_write_to_block (void* kpage)
{
  ASSERT (is_kernel_vaddr (kpage));

  /* Scan to find an empty space for a page. */
  size_t swap_index = bitmap_scan_and_flip (swap_bitmap, 0, 1, true);
  if (swap_index == BITMAP_ERROR)
    PANIC ("Error: Can't find an empty space for a page swap.");

  /* Each write will write a sector of 512 bytes, therefore we need to write
       SECTORS_PER_PAGE times, each at 512 increments of the kpage. */
  for (int i = 0; i < SECTORS_PER_PAGE; i++)
    block_write (global_swap_block,
                 swap_index * SECTORS_PER_PAGE + i,
                 kpage + (i * BLOCK_SECTOR_SIZE));

  return swap_index;
}

/** Deallocate a page size of area in global_swap_block.
    NOT ZEROING OUT! */
void
vm_swap_free (swap_index_t index)
{
  ASSERT (index < MAX_SWAP_PAGES_NUM);

  if (bitmap_test (swap_bitmap, index))
    PANIC ("Error, invalid free request to unassigned swap block");

  bitmap_set (swap_bitmap, index, true);
}