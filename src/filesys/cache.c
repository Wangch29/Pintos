/**
 * @brief This file contains the implementation of the buffer cache.
 *
 * This file contains the implementation of the buffer cache. The buffer cache
 * is responsible for caching blocks of data from the disk into memory
 * to improve the performance of the system.
 *
 */

#include "cache.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "devices/timer.h"

/** The number of blocks in buffer cache. */
#define CACHE_BLOCK_NUMBER 64

/**
 * @brief Cache block entry structure.
 *
 * This structure represents an entry in the cache. It contains information
 * about the block, including its sector index, validity, dirty flag, data, and
 * usage status.
 */
struct cache_block_entry
  {
    block_sector_t sector_idx;         /**< Sector index in disk.        */

    bool valid;                        /**< Entry validity.              */
    bool dirty;                        /**< Entry dirty bit.             */
    bool used;                         /**< Used recently.               */

    /* Sync. */
    struct lock entry_lock;            /**< Per entry lock.              */
    struct condition read_possible;    /**< Read condition variable.     */
    struct condition write_possible;   /**< Write condition variable.    */
    struct condition safe_to_evict;    /**< Eviction condition variable. */
    int readers;                       /**< Number of readers.           */
    int writers;                       /**< Number of writers.           */
    int waiting_readers;               /**< Number of waiting readers.   */
    int waiting_writers;               /**< Number of waiting writers.   */

    uint8_t data[BLOCK_SECTOR_SIZE];   /**< Sector data.                 */
  };

/** Cache blocks array. */
static struct cache_block_entry cache_array[CACHE_BLOCK_NUMBER];

/** Buffer cache global lock. */
static struct lock buffer_cache_lock;

/** Clock algorithm index in cache_array, for LRU. */
static uint8_t clock_idx;

static struct cache_block_entry * find_entry (block_sector_t sector);
static void write_back (int index);
static struct cache_block_entry * find_empty (block_sector_t sector_idx);
static struct cache_block_entry * clock_eviction (block_sector_t sector_idx);

static void flush_periodically (void);
static void create_flush_thread (void);

/**
 * @brief Initialize the buffer cache.
 *
 * This function initializes the buffer cache. It sets up the cache array and
 * initializes the clock algorithm index.
 */
void
cache_init (void)
{
  lock_init (&buffer_cache_lock);
  for (int i = 0; i < CACHE_BLOCK_NUMBER; i++)
    {
      lock_init (&cache_array[i].entry_lock);
      cond_init (&cache_array[i].read_possible);
      cond_init (&cache_array[i].write_possible);
      cond_init (&cache_array[i].safe_to_evict);
      cache_array[i].readers = 0;
      cache_array[i].writers = 0;
      cache_array[i].waiting_readers = 0;
      cache_array[i].waiting_writers = 0;
      cache_array[i].valid = false;
    }
  create_flush_thread ();
}

/**
 * @brief Reads a chunk of data from the buffer cache.
 *
 * This function reads a chunk of data from the buffer cache. If the data is
 * present in the cache, it is returned directly. If the data is not present in
 * the cache, it is read from the block device and stored in the cache.
 *
 * @param block The block device to read from.
 * @param sector The sector index to read.
 * @param buffer A pointer to the buffer to store the data in.
 * @param sector_ofs The offset within the sector to start reading from.
 * @param chunk_size The number of bytes to read.
 */
void
cache_read (block_sector_t sector, void *buffer, int sector_ofs, int chunk_size)
{
  ASSERT(sector_ofs + chunk_size <= BLOCK_SECTOR_SIZE);

  lock_acquire (&buffer_cache_lock);

  struct cache_block_entry *entry = find_entry (sector);
  /* Cache miss. */
  if (entry == NULL)
    entry = find_empty (sector);

  lock_acquire (&entry->entry_lock);
  lock_release (&buffer_cache_lock);
  entry->waiting_readers++;
  /* Wait while a writer is writing or waiting to write. */
  while (entry->writers > 0 || entry->waiting_writers > 0)
    cond_wait (&entry->read_possible, &entry->entry_lock);
  entry->waiting_readers--;
  entry->readers++;
  lock_release (&entry->entry_lock);

  memcpy (buffer, entry->data + sector_ofs, chunk_size);

  lock_acquire (&entry->entry_lock);
  entry->used = true;
  entry->readers--;
  /* If this is the last reader, signal possible waiting writers. */
  if (entry->readers == 0 && entry->waiting_writers > 0)
    cond_signal (&entry->write_possible, &entry->entry_lock);
  else if (entry->readers == 0 && entry->waiting_writers == 0)
    cond_signal (&entry->safe_to_evict, &entry->entry_lock);

  lock_release (&entry->entry_lock);
}

/**
 * @brief Writes a chunk of data to the buffer cache.
 *
 * This function writes a chunk of data to the buffer cache. If the data is
 * present in the cache, it is updated. If the data is not present in the cache,
 * it is written to the cache and then to the block device.
 *
 * @param block The block device to write to.
 * @param sector The sector index to write.
 * @param buffer A pointer to the buffer containing the data to write.
 * @param sector_ofs The offset within the sector to start writing to.
 * @param chunk_size The number of bytes to write.
 */
void
cache_write (block_sector_t sector, void *buffer, int sector_ofs, int chunk_size)
{
  ASSERT(sector_ofs + chunk_size <= BLOCK_SECTOR_SIZE);

  lock_acquire (&buffer_cache_lock);

  struct cache_block_entry *entry = find_entry (sector);
  /* Cache miss. */
  if (entry == NULL)
    entry = find_empty (sector);

  lock_acquire (&entry->entry_lock);
  lock_release (&buffer_cache_lock);
  entry->waiting_writers++;
  while (entry->readers > 0 || entry->writers > 0)
    {
      cond_wait (&entry->write_possible, &entry->entry_lock);
    }
  entry->waiting_writers--;
  entry->writers++;
  lock_release (&entry->entry_lock);

  memcpy (entry->data + sector_ofs, buffer, chunk_size);

  lock_acquire (&entry->entry_lock);
  entry->used = true;
  entry->dirty = true;
  entry->writers--;

  if (entry->waiting_writers > 0)
    cond_signal (&entry->write_possible, &entry->entry_lock);
  else if (entry->waiting_readers > 0)
    cond_broadcast (&entry->read_possible, &entry->entry_lock);
  else
    cond_signal (&entry->safe_to_evict, &entry->entry_lock);
  lock_release (&entry->entry_lock);
}

/**
 * @brief Writes back all modified buffer cache entries to storage.
 *
 * Ensures data consistency by flushing dirty cache entries, which have been
 * modified in memory, back to their storage locations. Typically called to
 * maintain data integrity, especially before system shutdown or after significant
 * file system operations. It locks the buffer cache during operation to prevent
 * concurrent modifications, iterating through each cache entry to check and
 * write back if dirty and valid, then releases the lock afterward.
 */
void
cache_flush (void)
{
  lock_acquire (&buffer_cache_lock);

  for (int i = 0; i < CACHE_BLOCK_NUMBER; i++)
    {
      if (cache_array[i].valid && cache_array[i].dirty)
        write_back (i);
    }

  lock_release (&buffer_cache_lock);
}

/**
 * Running an infinite loop, flush cache periodically for every 30 seconds.
 */
static void
flush_periodically (void)
{
  while (1)
    {
      timer_msleep (30 * 1000);
      cache_flush ();
    }
}

/**
 * Create a kernel thread to flush buffer cache periodically.
 */
static void
create_flush_thread (void)
{
  thread_create ("cache_flush", PRI_DEFAULT, flush_periodically, NULL);
}

/**
 * @brief Write back a cache entry to the block device.
 *
 * This function writes back a cache entry to the block device if it is dirty.
 * It ensures that the cache entry is valid before writing back to the block
 * device.
 *
 * @param index The index of the cache entry to write back.
 */
static void
write_back (int index)
{
  struct cache_block_entry *entry = &cache_array[index];
  block_write (fs_device, entry->sector_idx, entry->data);
  entry->dirty = false;
}

/**
 * Search buffer cache for sector_idx == sector.
 * @param sector the searched sector index.
 * @return pointer to the cache_block_entry.
 */
static struct cache_block_entry *
find_entry (block_sector_t sector)
{
  for (int i = 0; i < CACHE_BLOCK_NUMBER; i++)
    if (cache_array[i].valid && cache_array[i].sector_idx == sector)
      return &cache_array[i];

  return NULL;
}

/**
 * @brief Searches for an empty cache entry.
 *
 * This function searches for the first available cache entry in the cache.
 * If an entry is found, it is marked as valid and returned. If no entry is
 * found, clock_eviction() is called, and returns an empty cache_block_entry.
 *
 * @return A pointer to the first available cache entry.
 */
static struct cache_block_entry *
find_empty (block_sector_t sector_idx)
{
  /* Search for an empty entry. */
  for (int i = 0; i < CACHE_BLOCK_NUMBER; i++)
    {
      if (!cache_array[i].valid)
        {
          struct cache_block_entry * entry = &cache_array[i];
          entry->sector_idx = sector_idx;
          block_read (fs_device, sector_idx, entry->data);
          entry->dirty = false;
          entry->valid = true;
          return &cache_array[i];
        }
    }

  /* Eviction and get an empty entry. */
  return clock_eviction (sector_idx);
}

/**
 * @brief Clock algorithm for LRU eviction.
 *
 * This function implements the clock algorithm for LRU eviction. It searches
 * for the least recently used cache entry and evicts it.
 *
 * @return A pointer to the evicted cache entry, or NULL if no entry was evicted.
 */
static struct cache_block_entry *
clock_eviction (block_sector_t sector_idx)
{
  ASSERT (lock_held_by_current_thread (&buffer_cache_lock));

  struct cache_block_entry *entry = NULL;

  for (int i = 0; i < 2 * CACHE_BLOCK_NUMBER; i++)
    {
      if (!cache_array[clock_idx].used)
        {
          entry = &cache_array[clock_idx];

          lock_acquire (&entry->entry_lock);
          entry->valid = false;
          while (entry->readers > 0 || entry->writers > 0 || entry->waiting_writers > 0)
            cond_wait (&entry->safe_to_evict, &entry->entry_lock);

          if (cache_array[clock_idx].dirty)
            write_back (clock_idx);

          block_read (fs_device, sector_idx, entry->data);
          entry->dirty = false;
          entry->sector_idx = sector_idx;
          entry->valid = true;
          entry->used = true;

          lock_release (&entry->entry_lock);

          clock_idx = (clock_idx + 1) % CACHE_BLOCK_NUMBER;
          break;
        }
      cache_array[clock_idx].used = false;
      clock_idx = (clock_idx + 1) % CACHE_BLOCK_NUMBER;
    }

  return entry;
}





