#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/** Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/** The total number of blocks in an inode.
 *
 *  Warning: if you change that, you should change the size of unused to
 *  make sure sizeof (struct inode_disk) == BLOCK_SECTOR_SIZE.
 */
#define INODE_BLOCK_NUMBER 12

/** The number of direct blocks in an inode. */
#define DIRECT_BLOCK_NUMBER 10

/** The number of sectors in an indirect block. */
#define INDIRECT_BLOCK_SIZE (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

/** On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE (512) bytes long.

   Default:
   10 direct node:                  10 * 512B = 5KB (5120 B)
   1 indirect node:                128 * 512B = 64KB (65536 B)
   1 double indirect node:   128 * 128 * 512B = 8192KB ()

   Total: 8.12MB
   */
struct inode_disk {
  block_sector_t direct_blocks[DIRECT_BLOCK_NUMBER];  /**< Direct block sector numbers.         */
  block_sector_t indirect_block;                      /**< Indirect block sector number.        */
  block_sector_t double_indirect_block;               /**< Double indirect block sector number. */

  bool is_dir;                                        /**< Is a directory or not.               */
  off_t length;                                       /**< File size in bytes.                  */
  unsigned magic;                                     /**< Magic number.                        */
  uint32_t unused[113];                               /**< Not used.                            */
};

/** List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/** Lock for list open_inodes. */
static struct lock open_inodes_lock;

/** An all-zeros array with size of BLOCK_SECTOR_SIZE. */
static char zeros[BLOCK_SECTOR_SIZE];

static bool allocate_blocks (size_t sector_cnt, struct inode_disk *inode);
static bool allocate_sector (struct inode *inode, off_t offset);

static void init_sync (struct inode *inode);

/** Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t
size)
{
return
DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE
);
}

/** In-memory inode. */
struct inode {
  struct list_elem elem;              /**< Element in inode list.            */
  block_sector_t sector;              /**< Sector number of disk location.   */
  int open_cnt;                       /**< Number of openers.                */
  bool removed;                       /**< True if deleted, false otherwise. */
  int deny_write_cnt;                 /**< 0: writes ok, >0: deny writes.    */

  /* Sync. */
  struct lock inode_lock;             /**< */
  struct condition read_possible;
  struct condition write_possible;
  struct condition extend_possible;
  size_t readers;
  size_t writers;
  size_t extenders;
  size_t waiting_readers;
  size_t waiting_writers;
  size_t waiting_extenders;           /**< Waiting extenders. */

  struct inode_disk data;             /**< Inode content. */
};

/**
 * @brief Returns the block device sector that contains byte offset POS within INODE.
 * @param inode The inode to search.
 * @param pos The position to search for.
 * @return The block device sector that contains byte offset POS within INODE,
 *         or -1 if pos is out of file length range.
 */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);

  if (pos < 0 || pos >= inode->data.length)
    return -1;

  block_sector_t sectors = pos / BLOCK_SECTOR_SIZE;

  /* Direct blocks. */
  if (sectors < DIRECT_BLOCK_NUMBER)
    return inode->data.direct_blocks[sectors];

  /* Indirect block. */
  if (sectors < DIRECT_BLOCK_NUMBER + INDIRECT_BLOCK_SIZE)
    {
      block_sector_t indirect_block[INDIRECT_BLOCK_SIZE];

      if (inode->data.indirect_block == 0)
        return 0;

      cache_read (inode->data.indirect_block, indirect_block, 0, BLOCK_SECTOR_SIZE);
      return indirect_block[sectors - DIRECT_BLOCK_NUMBER];
    }

  /* Double indirect block. */
  if (sectors < DIRECT_BLOCK_NUMBER + INDIRECT_BLOCK_SIZE + INDIRECT_BLOCK_SIZE * INDIRECT_BLOCK_SIZE)
    {
      block_sector_t double_indirect_block[INDIRECT_BLOCK_SIZE];
      block_sector_t indirect_block[INDIRECT_BLOCK_SIZE];

      off_t double_index = (sectors - DIRECT_BLOCK_NUMBER - INDIRECT_BLOCK_SIZE)
                           / INDIRECT_BLOCK_SIZE;
      off_t indirect_index = (sectors - DIRECT_BLOCK_NUMBER - INDIRECT_BLOCK_SIZE)
                             % INDIRECT_BLOCK_SIZE;

      ASSERT (double_index < INDIRECT_BLOCK_SIZE && indirect_index < INDIRECT_BLOCK_SIZE);

      if (inode->data.double_indirect_block == 0)
        return 0;

      cache_read (inode->data.double_indirect_block, double_indirect_block, 0, BLOCK_SECTOR_SIZE);

      if (double_indirect_block[double_index] == 0)
        return 0;

      cache_read (double_indirect_block[double_index], indirect_block, 0, BLOCK_SECTOR_SIZE);

      return indirect_block[indirect_index];
    }

  NOT_REACHED ();
  return -1;
}

/** Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/** Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.

   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->is_dir = is_dir;
      disk_inode->magic = INODE_MAGIC;
      if (allocate_blocks (sectors, disk_inode))
        {
          cache_write (sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
          success = true;
        }
      free (disk_inode);
    }
  return success;
}

/**
 * @brief Initializes the synchronization structures for an inode.
 *
 * @param[in,out] inode The inode to initialize synchronization structures for.
 */
static void
init_sync (struct inode *inode)
{
  lock_init (&inode->inode_lock);
  cond_init (&inode->read_possible);
  cond_init (&inode->write_possible);
  cond_init (&inode->extend_possible);
  inode->readers = 0;
  inode->writers = 0;
  inode->extenders = 0;
  inode->waiting_readers = 0;
  inode->waiting_writers = 0;
  inode->waiting_extenders = 0;
}

/** Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  lock_acquire (&open_inodes_lock);
  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e,
      struct inode, elem);
      if (inode->sector == sector)
        {
          lock_release (&open_inodes_lock);
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  lock_release (&open_inodes_lock);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  init_sync (inode);
  cache_read (inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
  return inode;
}

/** Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/** Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/** Closes INODE and writes it to disk.

   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      lock_acquire (&open_inodes_lock);
      list_remove (&inode->elem);
      lock_release (&open_inodes_lock);

      /* Deallocate blocks if file is removed. */
      if (inode->removed)
        {
          struct inode_disk *data = &inode->data;
          /* Release direct blocks. */
          for (int i = 0; i < DIRECT_BLOCK_NUMBER; i++)
            {
              if (data->direct_blocks[i] == 0)
                continue;
              free_map_release (data->direct_blocks[i], 1);
            }

          /* Release indirect block. */
          if (data->indirect_block != 0)
            {
              block_sector_t indirect_block[INDIRECT_BLOCK_SIZE];
              cache_read (data->indirect_block, indirect_block, 0, BLOCK_SECTOR_SIZE);
              for (int i = 0; i < INDIRECT_BLOCK_SIZE; i++)
                {
                  if (indirect_block[i] == 0)
                    continue;
                  free_map_release (indirect_block[i], 1);
                }
              free_map_release (data->indirect_block, 1);
            }

          /* Release double indirect block. */
          if (data->double_indirect_block != 0)
            {
              block_sector_t double_indirect_block[INDIRECT_BLOCK_SIZE];
              cache_read (data->double_indirect_block, double_indirect_block, 0, BLOCK_SECTOR_SIZE);

              for (int i = 0; i < INDIRECT_BLOCK_SIZE; i++)
                {
                  if (double_indirect_block[i] == 0)
                    continue;

                  block_sector_t indirect_block[INDIRECT_BLOCK_SIZE];
                  cache_read (double_indirect_block[i], indirect_block, 0, BLOCK_SECTOR_SIZE);

                  for (int j = 0; j < INDIRECT_BLOCK_SIZE; j++)
                    {
                      if (indirect_block[j] == 0)
                        continue;
                      free_map_release (indirect_block[j], 1);
                    }
                  free_map_release (double_indirect_block[i], 1);
                }

              free_map_release (data->double_indirect_block, 1);
            }
        }

      free (inode);
    }
}

/** Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/** Return if the inode is removed. */
bool
inode_is_removed (const struct inode *inode)
{
  return inode->removed;
}

/** Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 *
@param inode The inode to read from.
@param buffer A pointer to the buffer to read into.
@param size The number of bytes to read.
@param offset The offset within the inode to start reading from.
@return The number of bytes actually read, which may be less
        than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  /* Read guarder. */
  lock_acquire (&inode->inode_lock);
  inode->waiting_readers++;
  if (!inode->data.is_dir)
    /* Normal file. */
    {
      while (inode->extenders > 0 || inode->waiting_extenders > 0)
        cond_wait (&inode->read_possible, &inode->inode_lock);
    }
  else
    /* Directory. */
    {
      while (inode->writers > 0 || inode->waiting_extenders > 0)
        cond_wait (&inode->read_possible, &inode->inode_lock);
    }
  inode->waiting_readers--;
  inode->readers++;
  lock_release (&inode->inode_lock);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Not-allocated sector, default to be zero. */
      if (sector_idx == 0)
        memcpy (buffer + bytes_read, zeros, chunk_size);
      else
        cache_read (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  lock_acquire (&inode->inode_lock);
  inode->readers--;
  if (!inode->data.is_dir)
    /* Normal file. */
    {
      if (inode->readers == 0 && inode->writers == 0 && inode->waiting_extenders > 0)
        cond_signal (&inode->extend_possible, &inode->inode_lock);
    }
  else
    /* Directory. */
    {
      if (inode->readers == 0 && inode->waiting_writers > 0)
        cond_signal (&inode->write_possible, &inode->inode_lock);
    }
  lock_release (&inode->inode_lock);

  return bytes_read;
}

/** Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  /* Read guarder. */
  lock_acquire (&inode->inode_lock);

  if (inode->deny_write_cnt)
    {
      lock_release (&inode->inode_lock);
      return 0;
    }

  bool is_extender = size + offset > inode_length (inode);

  if (!inode->data.is_dir)
    /* Normal file. */
    {
      if (is_extender)  // is extender.
        {
          inode->waiting_extenders++;
          while (inode->readers > 0 || inode->writers > 0 || inode->extenders > 0)
            cond_wait (&inode->extend_possible, &inode->inode_lock);
          inode->waiting_extenders--;
          inode->extenders++;
        }
      else  // normal writer.
        {
          inode->waiting_writers++;
          while (inode->extenders > 0 || inode->waiting_extenders > 0)
            cond_wait (&inode->write_possible, &inode->inode_lock);
          inode->waiting_writers--;
          inode->writers++;
        }
    }
  else
    /* Directory. */
    {
      inode->waiting_writers++;
      while (inode->readers > 0 || inode->writers > 0)
        cond_wait (&inode->write_possible, &inode->inode_lock);
      inode->waiting_writers--;
      inode->writers++;
    }
  lock_release (&inode->inode_lock);

  /* If writing position exceeds EOF, extends the file. */
  if (offset + size > inode_length (inode))
    {
      inode->data.length = offset + size;
      /* Write back the updated length. */
      cache_write (inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
    }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = -1u;
      sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      ASSERT (sector_idx != -1u);

      /* If sector_idx is 0, allocate a new sector. */
      while (sector_idx == 0)
        {
          if (!allocate_sector (inode, offset))
            PANIC ("Fatal: couldn't allocate sector.");

          cache_write (inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
          sector_idx = byte_to_sector (inode, offset);
        }
      ASSERT (sector_idx != 0);

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_write (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  lock_acquire (&inode->inode_lock);
  if (!inode->data.is_dir)
    /* Normal file. */
    {
      if (is_extender)  // is extender.
        {
          inode->extenders--;
          if (inode->waiting_extenders > 0)
            cond_signal (&inode->extend_possible, &inode->inode_lock);
          else if (inode->waiting_readers > 0 || inode->waiting_writers > 0)
            {
              cond_broadcast (&inode->write_possible, &inode->inode_lock);
              cond_broadcast (&inode->read_possible, &inode->inode_lock);
            }
        }
      else  // normal write.
        {
          inode->writers--;
          if (inode->readers == 0 && inode->writers == 0 && inode->waiting_extenders > 0)
            cond_signal (&inode->extend_possible, &inode->inode_lock);
        }
    }
  else
    /* Directory. */
    {
      inode->writers--;
      if (inode->waiting_writers > 0)
        cond_signal (&inode->write_possible, &inode->inode_lock);
      else if (inode->waiting_readers > 0)
        cond_broadcast (&inode->read_possible, &inode->inode_lock);
    }
  lock_release (&inode->inode_lock);

  return bytes_written;
}

/** Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  lock_acquire (&inode->inode_lock);
  inode->deny_write_cnt++;
  lock_release (&inode->inode_lock);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/** Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_acquire (&inode->inode_lock);
  inode->deny_write_cnt--;
  lock_release (&inode->inode_lock);
}

/** Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/**
 * @brief Returns whether the given inode is a directory or not.
 *
 * @param[in] inode The inode to check.
 * @return True if the inode is a directory, false otherwise.
 */
bool
inode_is_dir (const struct inode *inode)
{
  return inode->data.is_dir;
}

/**
 * @brief Returns the number of openers of an inode.
 * @param[in] inode The inode to check.
 * @return The number of openers of the inode.
 */
int
inode_get_open_cnt (const struct inode *inode)
{
  return inode->open_cnt;
}

/**
 * @brief Allocates blocks for an inode_disk.
 *
 * @param[in] sector_cnt The number of sectors to allocate.
 * @param[in,out] inode The inode to allocate blocks for.
 * @return True if the blocks were allocated, false otherwise.
 */
static bool
allocate_blocks (size_t sector_cnt, struct inode_disk *inode)
{
  block_sector_t rest = sector_cnt;

  if (rest == 0)
    return true;  // Create empty file.

  /* First, allocate direct blocks. */
  block_sector_t *direct_blocks = inode->direct_blocks;
  for (int i = 0; i < DIRECT_BLOCK_NUMBER; i++)
    {
      free_map_allocate (1, direct_blocks + i);
      if (--rest == 0)
        return true;
    }

  /* Second, allocate indirect block. */
  {
    block_sector_t buffer[INDIRECT_BLOCK_SIZE];
    memcpy (buffer, zeros, BLOCK_SECTOR_SIZE);
    free_map_allocate (1, &inode->indirect_block);
    for (int i = 0; i < INDIRECT_BLOCK_SIZE; i++)
      {
        free_map_allocate (1, buffer + i);
        if (--rest == 0)
          {
            cache_write (inode->indirect_block, buffer, 0, BLOCK_SECTOR_SIZE);
            return true;
          }
      }
    cache_write (inode->indirect_block, buffer, 0, BLOCK_SECTOR_SIZE);
  }

  /* Thirdly, allocate double indirect block. */
  free_map_allocate (1, &inode->double_indirect_block);
  block_sector_t first_buffer[INDIRECT_BLOCK_SIZE] = {0};

  for (int i = 0; i < INDIRECT_BLOCK_SIZE; i++)
    {
      block_sector_t second_buffer[INDIRECT_BLOCK_SIZE] = {0};
      free_map_allocate (1, first_buffer + i);

      for (int j = 0; j < INDIRECT_BLOCK_SIZE; j++)
        {
          free_map_allocate (1, second_buffer + j);
          if (--rest == 0)
            {
              cache_write (first_buffer[i], second_buffer, 0, BLOCK_SECTOR_SIZE);
              cache_write (inode->double_indirect_block, first_buffer, 0, BLOCK_SECTOR_SIZE);
              return true;
            }
        }

      cache_write (first_buffer[i], second_buffer, 0, BLOCK_SECTOR_SIZE);
    }
  cache_write (inode->double_indirect_block, first_buffer, 0, BLOCK_SECTOR_SIZE);
  return true;
}

/**
 * @brief Allocates a single sector for an inode_disk.
 *
 * @param[in,out] inode The inode to allocate blocks for.
 * @param[in] offset The offset of the sector to allocate.
 * @return True if the sector were allocated, false otherwise.
 */
static bool
allocate_sector (struct inode *inode, off_t offset)
{
  ASSERT (inode != NULL);

  block_sector_t sector_idx = offset / BLOCK_SECTOR_SIZE; //bytes_to_sectors (offset);

  /* Check if exceeds file length. */
  if (offset >= inode->data.length)
    return false;

  /* Direct blocks. */
  if (sector_idx < DIRECT_BLOCK_NUMBER)
    {
      if (inode->data.direct_blocks[sector_idx] != 0)
        return false;
      free_map_allocate (1, inode->data.direct_blocks + sector_idx);
    }

    /* Indirect block. */
  else if (sector_idx < INDIRECT_BLOCK_SIZE + DIRECT_BLOCK_NUMBER)
    {
      block_sector_t indirect_block[INDIRECT_BLOCK_SIZE];
      block_sector_t block_idx = sector_idx - DIRECT_BLOCK_NUMBER;

      if (inode->data.indirect_block == 0)
        {
          free_map_allocate (1, &inode->data.indirect_block);
          memcpy (indirect_block, zeros, BLOCK_SECTOR_SIZE);
        }
      else
        cache_read (inode->data.indirect_block, indirect_block, 0, BLOCK_SECTOR_SIZE);

      if (indirect_block[block_idx] != 0)
        return false;

      free_map_allocate (1, indirect_block + block_idx);

      cache_write (inode->data.indirect_block, indirect_block, 0, BLOCK_SECTOR_SIZE);
    }

    /* Double indirect block. */
  else if (sector_idx < DIRECT_BLOCK_NUMBER + INDIRECT_BLOCK_SIZE + INDIRECT_BLOCK_SIZE * INDIRECT_BLOCK_SIZE)
    {
      block_sector_t double_indirect_block[INDIRECT_BLOCK_SIZE];
      block_sector_t indirect_block[INDIRECT_BLOCK_SIZE];

      block_sector_t double_index = (sector_idx - DIRECT_BLOCK_NUMBER - INDIRECT_BLOCK_SIZE)
                                    / INDIRECT_BLOCK_SIZE;
      block_sector_t indirect_index = (sector_idx - DIRECT_BLOCK_NUMBER - INDIRECT_BLOCK_SIZE)
                                      % INDIRECT_BLOCK_SIZE;

      /* Load double indirect block. */
      if (inode->data.double_indirect_block == 0)  // Not allocate double indirect block yet.
        {
          free_map_allocate (1, &inode->data.double_indirect_block);
          memcpy (double_indirect_block, zeros, BLOCK_SECTOR_SIZE);
        }
      else
        cache_read (inode->data.double_indirect_block, double_indirect_block, 0, BLOCK_SECTOR_SIZE);

      /* Load indirect block. */
      if (double_indirect_block[double_index] == 0)
        {
          free_map_allocate (1, double_indirect_block + double_index);
          memcpy (indirect_block, zeros, BLOCK_SECTOR_SIZE);
        }
      else
        cache_read (double_indirect_block[double_index], indirect_block, 0, BLOCK_SECTOR_SIZE);

      if (indirect_block[indirect_index] != 0)
        return false;

      free_map_allocate (1, indirect_block + indirect_index);

      cache_write (double_indirect_block[double_index], indirect_block, 0, BLOCK_SECTOR_SIZE);
      cache_write (inode->data.double_indirect_block, double_indirect_block, 0, BLOCK_SECTOR_SIZE);
    }

  return true;
}
