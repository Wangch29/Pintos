#include "frametable.h"
#include <list.h>
#include <hash.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/swap.h"
#include "vm/suppagetable.h"

/** Frame table lock. */
static struct lock frame_table_lock;

/** Frame hash table mapping frames to frame entries. */
static struct hash frame_table_hash;

/** A circuit list that contains an entry for each frame that contains a user page. */
static struct list frame_table_list;

/** The clock pointer for clock algorithm. */
static struct list_elem* clock_ptr;

void frametable_eviction (void);
struct frame_table_entry* frametable_clock_eviction (uint32_t* pagedir);
void vm_frametable_set_pin (void *kpage, bool pin);

/** Hash table functions. */
static unsigned frametable_hash_func (const struct hash_elem *elem, void *aux UNUSED);
static bool frametable_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);

/** Frame table entry. */
struct frame_table_entry
{
    void* kpage;                  /**< Kernel page pointer.           */
    void* upage;                  /**< User page pointer.             */
    struct thread* owner;         /**< Its owner thread.              */

    struct hash_elem h_elem;      /**< Elem for frame_table_hash.     */
    struct list_elem l_elem;      /**< Elem for frame_table_list.     */

    bool pin;                     /**< Pinned to avoid being evicted. */
};

/** Initialize frame table. */
void
vm_frametable_init ()
{
  lock_init (&frame_table_lock);
  hash_init (&frame_table_hash, frametable_hash_func, frametable_less_func, NULL);
  list_init (&frame_table_list);
  clock_ptr = NULL;
}

/** Allocate a new frame.

    flags: the palloc_flag.
    upage: the user page.
    Returns the address of allocated kernel page. */
void*
vm_frametable_allocate (enum palloc_flags flags, void *upage)
{
  void* new_kpage;
  struct frame_table_entry* fte;

  lock_acquire (&frame_table_lock);

  /* Get a new kpage. */
  new_kpage = palloc_get_page (PAL_USER | flags);
  if (new_kpage == NULL)
    /* Eviction. */
    {
      frametable_eviction ();
      /* Get new kpage. */
      new_kpage = palloc_get_page (PAL_USER | flags);
      ASSERT (new_kpage != NULL);
    }

  /* Malloc and initializes a new page table entry. */
  fte = (struct frame_table_entry*) malloc (sizeof (struct frame_table_entry));
  if (fte == NULL)
  {
    palloc_free_page (new_kpage);
    lock_release (&frame_table_lock);
    return NULL;
  }
  fte->kpage = new_kpage;
  fte->owner = thread_current ();
  fte->upage = upage;
  fte->pin = true;
  hash_insert (&frame_table_hash, &fte->h_elem);
  list_push_back (&frame_table_list, &fte->l_elem);

  lock_release (&frame_table_lock);

  return new_kpage;
}

/** Deallocate a page‘s entry and its page. */
void
vm_frametable_free (void *kpage)
{
  lock_acquire (&frame_table_lock);
  vm_frametable_do_free (kpage, true);
  lock_release (&frame_table_lock);
}

/** Deallocate a page's entry without free the page. */
void
vm_frametable_free_entry (void *kpage)
{
  lock_acquire (&frame_table_lock);
  vm_frametable_do_free (kpage, false);
  lock_release (&frame_table_lock);
}

/** Do free a frame, a helper function in frametable.c.
    Must run with frame_table_lock!

    kpage is the page whose entry will be freed.
    free_page determines if the page will be freed. */
void
vm_frametable_do_free (void *kpage, bool free_page)
{
  ASSERT (lock_held_by_current_thread(&frame_table_lock) == true);
  ASSERT (is_kernel_vaddr(kpage));
  ASSERT (pg_ofs (kpage) == 0);

  struct hash_elem* he;
  struct frame_table_entry* fte;
  struct frame_table_entry fte_temp;

  fte_temp.kpage = kpage;
  he = hash_find (&frame_table_hash, &fte_temp.h_elem);
  if (he == NULL)
  {
    PANIC ("This page:%p is not in frame table.", kpage);
  }

  fte = (struct frame_table_entry *) hash_entry (he, struct frame_table_entry, h_elem);
  hash_delete (&frame_table_hash, &fte->h_elem);
  list_remove (&fte->l_elem);

  if (free_page)
    palloc_free_page (fte->kpage);
  free (fte);
}

/**
 *  Pin a frame to protect from eviction.
 */
void
vm_frametable_pin (void *kpage)
{
  vm_frametable_set_pin (kpage, true);
}

/**
 *  Unpin a frame.
 */
void
vm_frametable_unpin (void *kpage)
{
  vm_frametable_set_pin (kpage, false);
}

/** Set kpage 's frame_table_entry's pin. */
vm_frametable_set_pin (void *kpage, bool pin)
{
  lock_acquire (&frame_table_lock);

  struct frame_table_entry fte_temp;
  fte_temp.kpage = kpage;
  struct hash_elem* e = hash_find (&frame_table_hash, &fte_temp.h_elem);
  if (e == NULL)
    PANIC ("Cannot unpin, because this page:%p is not in frame table.", kpage);

  struct frame_table_entry* fte = hash_entry (e, struct frame_table_entry, h_elem);
  fte->pin = pin;

  lock_release (&frame_table_lock);
}

/** Evict a frame. */
void
frametable_eviction (void)
{
  struct thread* cur = thread_current ();

  struct frame_table_entry* evicted_fte = frametable_clock_eviction (cur->pagedir);
  ASSERT (evicted_fte != NULL);
  ASSERT (evicted_fte->owner != NULL);

  struct sup_page_table_entry* spte = vm_spt_find_page
          (evicted_fte->owner->sup_page_table, evicted_fte->upage);
  if (spte == NULL)
    PANIC ("vm_frametable_allocate: cannot find sup_page_table");

  /* Mark as not-present. */
  pagedir_clear_page (evicted_fte->owner->pagedir, evicted_fte->upage);

  /* Reset sup_page_table_entry of evicted fte. */
  spte->swap_index = vm_swap_write_to_block (evicted_fte->kpage);
  spte->kpage = NULL;
  spte->pstatus = ON_SWAP;
  spte->dirty = spte->dirty
                || false
                || pagedir_is_dirty (evicted_fte->owner->pagedir, evicted_fte->upage)
                || pagedir_is_dirty (evicted_fte->owner->pagedir, evicted_fte->kpage); //TODO: What's that???

  /* Free fte. */
  hash_delete (&frame_table_hash, &evicted_fte->h_elem);
  list_remove (&evicted_fte->l_elem);
  palloc_free_page (evicted_fte->kpage);
  free (evicted_fte);
}

/** Return the next frame_table_entry to evict, using clock algorithm. */
struct frame_table_entry*
frametable_clock_eviction (uint32_t* pagedir)
{
  if (list_empty (&frame_table_list))
    PANIC ("Frame table is empty, can't happen - there is a leak somewhere.\n");

  size_t n = hash_size (&frame_table_hash);
  if (n == 0)
    PANIC("Frame table is empty, can't happen - there is a leak somewhere.\n");

  for (size_t i = 0; i < 2 * n; i++)
  {
    /* Turn to the next entry. */
    if (clock_ptr == list_end (&frame_table_list) || clock_ptr == NULL)
      clock_ptr = list_begin (&frame_table_list);
    else
      clock_ptr = list_next (clock_ptr);

    struct frame_table_entry *fte = list_entry (clock_ptr, struct frame_table_entry, l_elem);

    /* If pinned, continue. */
    if (fte->pin)
      continue;

    /* If entry has been accessed, reset and continue. */
    if (pagedir_is_accessed (pagedir, fte->upage))
      {
        pagedir_set_accessed (pagedir, fte->upage, false);
        continue;
      }
    return fte;
  }

  PANIC ("Cannot find empty memory to evict!\n");
}

/** Hash function for frame table. */
static unsigned
frametable_hash_func (const struct hash_elem* elem, void* aux UNUSED)
{
  const struct frame_table_entry *fte = hash_entry (elem, struct frame_table_entry, h_elem);
  return hash_bytes (&fte->kpage, sizeof (fte->kpage));
}

/** Hash less function for frame table. */
static bool
frametable_less_func (const struct hash_elem* a, const struct hash_elem* b, void* aux UNUSED)
{
  const struct frame_table_entry *fte_a = hash_entry (a, struct frame_table_entry, h_elem);
  const struct frame_table_entry *fte_b = hash_entry (b, struct frame_table_entry, h_elem);
  return fte_a->kpage < fte_b->kpage;
}

