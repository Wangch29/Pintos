#include "suppagetable.h"
#include <hash.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/frametable.h"

static unsigned spt_hash_func (const struct hash_elem *elem, void *aux UNUSED);
static bool spt_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
static void spt_destroy_func (struct hash_elem *elem, void *aux UNUSED);

/** Create a new supplemental page table, and return the pointer to it. */
struct sup_page_table*
vm_spt_create (void)
{
  struct sup_page_table* spt = (struct sup_page_table*) malloc (sizeof (struct sup_page_table));
  if (spt == NULL)
    return NULL;
  hash_init (&spt->page_table_hash, spt_hash_func, spt_less_func, NULL);
  return spt;
}

/** Destroy a supplemental page table without freeing its pages. */
void
vm_spt_destroy (struct sup_page_table* spt)
{
  if (spt == NULL)
    return;

  hash_destroy (&spt->page_table_hash, spt_destroy_func);
  free (spt);
}

/** Install a frame page to sup_page_table.
    This frame has been added to page table by pagedir_set_page ().

    Return true if succeeds, false otherwise. */
bool
vm_spt_install_frame (struct sup_page_table* spt, void* upage, void* kpage)
{
  struct sup_page_table_entry *spte;
  struct hash_elem *e;

  spte = (struct sup_page_table_entry *) malloc (sizeof (struct sup_page_table_entry));
  spte->upage = upage;
  spte->kpage = kpage;
  spte->pstatus = ON_FRAME;
  spte->dirty = false;
  spte->swap_index = -1;

  e = hash_insert (spt, &spte->elem);
  if (e == NULL)
    return true;
  else
    free (spte);
  return false;
}

/** Install a page to sup_page_table, whose status
    is ALL_ZERO.

    Return true if succeeds, false otherwise. */
bool
vm_spt_install_zeropage (struct sup_page_table* spt, void* upage)
{
  struct sup_page_table_entry *spte;
  struct hash_elem *e;

  spte = (struct sup_page_table_entry *) malloc (sizeof (struct sup_page_table_entry));
  spte->upage = upage;
  spte->kpage = NULL;
  spte->pstatus = ALL_ZERO;
  spte->dirty = false;
  spte->swap_index = -1;

  e = hash_insert (spt, &spte->elem);
  if (e == NULL)
    return true;

  /* There is already an entry -- impossible state. */
  PANIC("Duplicated SPT entry for zeropage");
  return false;
}

/** Find a page's entry in a supplemental page table.
    Return NULL if the page doesn't exist.

    spt is the pointer to the sup_page_table
    upage is the searched page. */
struct sup_page_table_entry*
vm_spt_find_page (struct sup_page_table *spt, void *upage)
{
  struct sup_page_table_entry temp;

  temp.upage = upage;

  struct hash_elem *e = hash_find (&spt->page_table_hash, &temp.elem);
  if (e == NULL)
    return NULL;
  return hash_entry (e, struct sup_page_table_entry, elem);
}

/** If sup_page_table has a page or not.

    spt is the pointer to the sup_page_table
    upage is the searched page. */
bool
vm_spt_has_page (struct sup_page_table *spt, void *upage)
{
  struct sup_page_table_entry* spte = vm_spt_find_page (spt, upage);
  return spte != NULL;
}

/** Set dirty bit of a page's entry.
    Return true if succeeds, false otherwise. */
bool
vm_spt_set_dirty (struct sup_page_table *spt, void *upage, bool dirty)
{
  struct sup_page_table_entry *spte = vm_spt_find_page (spt, upage);
  if (spte == NULL)
    return false;

  spte->dirty = spte->dirty || dirty; // TODO: can be simplified.
  return true;
}

/** Load a page's physical frame into memory.
    Return true if loading succeed, false otherwise. */
bool
vm_load_page (struct sup_page_table *spt, uint32_t *pagedir, void *upage)
{
  struct sup_page_table_entry *spte;
  void *new_kpage;
  bool writable = true;

  /* Find sup_page_table_entry. */
  spte = vm_spt_find_page (spt, upage);
  if (spte == NULL)
    return false;

  if (spte->pstatus == ON_FRAME)
    /* Already loaded. */
    return true;

  /* Allocate a new frame. */
  new_kpage = vm_frametable_allocate (PAL_USER, upage);
  if (new_kpage == NULL)
    return false;

  switch (spte->pstatus)
    {
      case ALL_ZERO:
        memset (new_kpage, 0, PGSIZE);
        break;

      case ON_SWAP:
        //TODO: to do swap.
        break;

      case FROM_FILESYS:
        //TODO: to be implemented.
        break;

      default:
        PANIC ("unreachable state");
    }

  /* Adds a mapping in page directory from user virtual page
     UPAGE to the physical frame identified by kernel virtual
     address new_kpage. */
  if (!pagedir_set_page (pagedir, upage, new_kpage, writable))
    {
      vm_frametable_free (new_kpage);
      return false;
    }

  spte->kpage = new_kpage;
  spte->pstatus = ON_FRAME;
  pagedir_set_dirty (pagedir, new_kpage, false);

  return true;
}























/** Hash function for supplemental page table. */
static unsigned
spt_hash_func (const struct hash_elem *elem, void *aux UNUSED)
{
  const struct sup_page_table_entry* spte = hash_entry (elem, struct sup_page_table_entry, elem);
  return hash_int ((int) spte->upage);
}

/** Hash less function for supplemental page table. */
static bool
spt_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  const struct sup_page_table_entry* spte_a = hash_entry (a, struct sup_page_table_entry, elem);
  const struct sup_page_table_entry* spte_b = hash_entry (b, struct sup_page_table_entry, elem);
  return spte_a->upage < spte_b->upage;
}

/** Hash destroy function for supplemental page table. */
static void
spt_destroy_func (struct hash_elem *elem, void *aux UNUSED)
{
  struct sup_page_table_entry* spte = hash_entry (elem, struct sup_page_table_entry, elem);

  /* Free associated frame entry. */
  if (spte->kpage != NULL)
    {
      ASSERT (spte->pstatus == ON_FRAME);
      vm_frametable_free_entry (spte->kpage);
    }
  else if (spte->pstatus == ON_SWAP)
    {
      //vm_swap_free (spte->swap_index);
    }

  free (spte);
}

