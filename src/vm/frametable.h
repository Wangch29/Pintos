#ifndef VM_FRAMETABLE_H
#define VM_FRAMETABLE_H

#include "threads/palloc.h"

void vm_frametable_table_init ();

void* vm_frametable_allocate (enum palloc_flags flags, void *upage);

void vm_frametable_free (void *kpage);
void vm_frametable_free_entry (void *kpage);

void vm_frametable_pin (void *kpage);
void vm_frametable_unpin (void *kpage);

#endif /**< vm/frametable.h */
