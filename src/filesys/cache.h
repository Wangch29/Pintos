#ifndef PINTOS_CACHE_H
#define PINTOS_CACHE_H

#include "devices/block.h"

/** Cache lifespan functions. */
void cache_init (void);

/** Cache read and write. */
void cache_read (block_sector_t sector, void *buffer, int sector_ofs, int chunk_size);
void cache_write (block_sector_t sector, void *buffer, int sector_ofs, int chunk_size);
void cache_flush (void);

#endif /**< filesys/cache.h */
