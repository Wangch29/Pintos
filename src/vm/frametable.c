#include "frametable.h"
#include <list.h>
#include <hash.h>
#include <stdio.h>
#include <stdbool.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include <stdint.h>

/** Frame table lock. */
static struct lock frame_table_lock;

/** Frame hash table mapping frames to frame entries. */
static struct hash frame_table_hash;

/** A circuit list that contains an entry for each frame that contains a user page. */
static struct list frame_table_list;

static unsigned frametable_hash_func (const struct hash_elem *elem, void *aux UNUSED);
static bool frametable_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);

/** Frame table entry. */
struct frame_table_entry
{
    void* kpage;                  /**< Kernel page pointer.    */
    void* upage;                  /**< User page pointer.      */
    struct thread* owner;         /**< Its owner thread.       */

    struct hash_elem h_elem;      /**< Elem for frame_table_hash.       */
    struct list_elem l_elem;      /**< Elem for frame_table_list.       */

};

/** Initialize frame table. */
void
vm_frametable_init ()
{
  lock_init (&frame_table_lock);
  hash_init (&frame_table_hash, frametable_hash_func, frametable_less_func, NULL);
  list_init (&frame_table_list);
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
    {
     return; //TODO: wait to be implemented.
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
  hash_insert (&frame_table_hash, &fte->h_elem);
  list_push_back (&frame_table_list, &fte->l_elem);

  lock_release (&frame_table_lock);

  //printf ("\nframe table insert:%p\n", new_kpage);

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

  //printf ("\nframe table remove:%p\n", kpage);
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

