#ifndef VM_SUPPAGETABLE_H
#define VM_SUPPAGETABLE_H

#include <hash.h>

/** Status of pages. */
enum page_status {
    ALL_ZERO,         /**< All zeros.                       */
    ON_FRAME,         /**< Actively in memory.              */
    ON_SWAP,          /**< Swapped (on swap slot).          */
    FROM_FILESYS      /**< from filesystem (or executable). */
};

/** Supplemental page table. */
struct sup_page_table
  {
    struct hash page_table_hash;
  };

/** Supplemental page table entry. */
struct sup_page_table_entry
  {
    void* upage;                 /**< User page.   */
    void* kpage;                 /**< Kernel page. */

    enum page_status pstatus;    /**< Page status. */
    void* swap_index;            /**< Swap index.  */

    bool dirty;                  /**< Dirty bit.   */

    struct hash_elem elem;       /**< Hash elem.   */
  };

/** Life cycle functions. */
struct sup_page_table* vm_spt_create (void);
void vm_spt_destroy (struct sup_page_table* spt);

/** Entry insert functions. */
bool vm_spt_install_frame (struct sup_page_table* spt, void* upage, void* kpage);
bool vm_spt_install_zeropage (struct sup_page_table* spt, void* upage);

/** Search functions. */
struct sup_page_table_entry* vm_spt_find_page (struct sup_page_table *spt, void *upage);
bool vm_spt_has_page (struct sup_page_table *spt, void *upage);

bool vm_spt_set_dirty (struct sup_page_table *spt, void *upage, bool dirty);

/** Page loading functions. */
bool vm_load_page (struct sup_page_table *spt, uint32_t *pagedir, void *upage);





#endif /**< vm/suppagetable.h */
