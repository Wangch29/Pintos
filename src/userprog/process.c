#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "vm/frametable.h"
#include "vm/suppagetable.h"

#ifndef VM
/* If not in VM, back to naive palloc. */
#define vm_frametable_allocate(x, y) palloc_get_page(x)
#define vm_frametable_free(x) palloc_free_page(x)
#endif

/* The max length in char of a token. */
#define MAX_TOKEN_LENGTH 128

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *process_args)
{
  char *file_name;
  char *process_args_copy1;
  char *process_args_copy2;
  char *saved_ptr;
  tid_t tid;
  struct thread *cur = thread_current ();

  /* process_args_copy1 stores the name of thread. */
  process_args_copy1 = palloc_get_page(0);
  /* process_args_copy2 stores the start_process () argument. */
  process_args_copy2 = palloc_get_page(0);
  if (process_args_copy1 == NULL || process_args_copy2 == NULL)
    {
      printf("[Error] Kernel Error: Not enough memory\n");
      palloc_free_page (process_args_copy1);
      palloc_free_page (process_args_copy2);
      return PID_ERROR;
    }

  strlcpy (process_args_copy1, process_args, PGSIZE);
  strlcpy (process_args_copy2, process_args, PGSIZE);

  /* Make a copy of FILE_NAME.
     Otherwise, there's a race between the caller and load(). */
  file_name = strtok_r (process_args_copy1, " ", &saved_ptr);

  /* Create a new thread. */
  struct process *p = malloc (sizeof (struct process));
  if (p == NULL)
    return PID_ERROR;
  p->cmd = process_args_copy2;
  p->pid = PID_INITIALIZING;
  p->exit = false;
  p->exit_status = 0; // TODO: ???
  p->parent_sleeping = false;
  p->parent = cur;
  sema_init (&p->sema, 0);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (file_name, PRI_DEFAULT, start_process, p);

  /* The process_args_copy1 stores the thread name, and after thread_create, the name
     is stored in struct thread, so I can free it. */
  palloc_free_page (process_args_copy1);

  if (tid == TID_ERROR)
    return PID_ERROR;

  /* Wait until child process get loaded. And check
     if loading succeed. */
  sema_down (&cur->wait_child_load);
  if (!cur->child_load_success)
    return PID_ERROR;
  /* Reset load flag. */
  cur->child_load_success = false;
  /* Insert to children_list after load succeeds. */
  list_push_back (&cur->children_list, &p->elem);

  return tid;
}

/** A thread function that loads a user process and starts it
   running. */
static void
start_process (void *process_)
{
  char *process_args = ((struct process*) process_)->cmd;
  char *process_args_copy;
  char *file_name;
  struct intr_frame if_;
  bool success = false;
  char *saved_ptr;
  struct thread *cur = thread_current ();

  cur->process = process_;
  cur->process->thread = cur;

  /* process_args_copy is used to store the file_name. */
  process_args_copy = palloc_get_page (0);
  if (process_args_copy == NULL)
    {
      palloc_free_page (process_args);
      cur->exit_status = -1;
      cur->process->exit = true;
      cur->process->parent->child_load_success = false;
      sema_up (&cur->process->parent->wait_child_load);
      thread_exit ();
    }
  strlcpy (process_args_copy, process_args, PGSIZE);
  file_name = strtok_r(process_args_copy, " ", &saved_ptr);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  lock_acquire (&filesys_lock);
  success = load (file_name, &if_.eip, &if_.esp);
  lock_release (&filesys_lock);
  /* If load failed, quit. */
  if (!success)
    {
      palloc_free_page (process_args);
      palloc_free_page (process_args_copy);
      /* Set exit_status to -1. */
      cur->exit_status = -1;
      cur->process->exit = true;
      cur->process->parent->child_load_success = false;
      /* Wake up parent process. */
      sema_up (&cur->process->parent->wait_child_load);
      thread_exit ();
    }

  /* Put arguments on the stack. */
  if_.esp = PHYS_BASE;
  char* words[MAX_TOKEN_LENGTH];
  uintptr_t argv0;
  uint8_t n = 0;
  uint32_t argc = 0;
  size_t ptr_size = sizeof (uintptr_t);

  /* Firstly, break the command into words. */
  for (char *token = strtok_r (process_args, " ", &saved_ptr); token != NULL;
       token = strtok_r (NULL, " ", &saved_ptr))
    *(words + n++) = token;
  argc = n;
  uintptr_t words_address[n];
  /* Secondly, put the words on the stack. */
  while (n != 0)
  {
    size_t size = strlen(*(words + --n)) + 1;
    if_.esp -= size;
    memcpy(if_.esp, *(words + n), size);
    words_address[n] = (uintptr_t) if_.esp;
  }
  /* Thirdly, word-align to a multiple of 4. */
  uint8_t word_align_num = (uint32_t) if_.esp & 0x03;
  if_.esp -= word_align_num;
  memset(if_.esp, 0, word_align_num);

  /* Fourthly, push the address of each string plus a null pointer sentinel,
     on the stack, in last-to-first order. */
  if_.esp -= ptr_size;
  memset(if_.esp, 0, ptr_size);
  for (n = 0; n != argc; n++)
  {
    if_.esp -= ptr_size;
    memcpy(if_.esp, &words_address[argc-1 - n], ptr_size);
  }
  argv0 = (uintptr_t) if_.esp;
  /* Fifthly, push argv (the address of argv[0]) and argc, in that order.   */
  if_.esp -= ptr_size;
  memcpy(if_.esp, &argv0, ptr_size);
  if_.esp -= sizeof(uint32_t);
  memcpy(if_.esp, &argc, sizeof(uint32_t));
  /* Finally, push a fake "return address".  */
  if_.esp -= ptr_size;
  memset(if_.esp, 0, ptr_size);

  cur->process->pid = cur->tid;

  //printf("STACK SET. ESP: %p\n", if_.esp);
  //hex_dump((uintptr_t)if_.esp, if_.esp, 100, true);

  palloc_free_page (process_args);
  palloc_free_page (process_args_copy);

  /* Tell its parent loading succeed, and wake it up! */
  cur->process->parent->child_load_success = true;
  sema_up (&cur->process->parent->wait_child_load);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/** Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 
{
  struct thread *current = thread_current ();

  for (struct list_elem *e = list_begin (&current->children_list);
        e != list_end (&current->children_list); e = list_next (e))
    {
      struct process *p = list_entry (e, struct process, elem);
      /* Search for child_tid. */
      if (p->pid == child_tid)
      {
        list_remove (e);
        /* If child process has exited. */
        if (p->exit)
          {
            int exit_status = p->exit_status;
            free (p); //TODO: ?
            return exit_status;
          }
        /* If child process has not exited, sleep. */
        p->parent_sleeping = true;
        sema_down (&p->sema);
        p->parent_sleeping = false;
        int exit_status = p->exit_status;
        free (p);  //TODO: ?
        return exit_status;
      }
    }
  return -1;
}

/** Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  struct process *p = cur->process;
  uint32_t *pd;

  printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  /* Close executing file. */
  if (cur->execute_file)
    {
      lock_acquire (&filesys_lock);
      file_allow_write (cur->execute_file);
      file_close (cur->execute_file);
      lock_release (&filesys_lock);
    }
  /* Close file_descriptor_table */
  lock_acquire (&filesys_lock);
  while (!list_empty (&cur->file_descriptor_table))
  {
    struct list_elem *e = list_pop_front (&cur->file_descriptor_table);
    struct file_table_entry *fte = list_entry(e, struct file_table_entry, elem);
    file_close (fte->file);
    free (fte);
  }
  lock_release (&filesys_lock);

  /* As a parent, set its children process's parent to NULL.
     If its child has existed, free its process. */
  for (struct list_elem *e = list_begin (&cur->children_list);
       e != list_end (&cur->children_list);)
  {
    struct process *child_p = list_entry (e, struct process, elem);
    e = list_remove (e);
    if (!child_p->exit)
      /* Set the child process's parent to NULL, if child haven't exit. */
      child_p->parent = NULL;
    else
      /* Free child process, if child has exited. */
      free (child_p);
  }

  /* As a child, update its process. */
  if (p->parent != NULL)
  {
    p->exit = true;
    //p->exit_status = cur->exit_status; //TODO: !
    p->thread = NULL;
    /* If parent_sleeping is true, wake parent up. */
    if (p->parent_sleeping)
      sema_up (&p->sema);
  }
  else
    /* When its parent has exited, free process. */
    free (cur->process);

#ifdef VM
  vm_spt_destroy (cur->sup_page_table);
  cur->sup_page_table = NULL; //TODO: ???
#endif

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/** Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/** We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/** ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** For use with ELF types in printf(). */
#define PE32Wx PRIx32   /**< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /**< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /**< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /**< Print Elf32_Half in hexadecimal. */

/** Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/** Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/** Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /**< Ignore. */
#define PT_LOAD    1            /**< Loadable segment. */
#define PT_DYNAMIC 2            /**< Dynamic linking info. */
#define PT_INTERP  3            /**< Name of dynamic loader. */
#define PT_NOTE    4            /**< Auxiliary info. */
#define PT_SHLIB   5            /**< Reserved. */
#define PT_PHDR    6            /**< Program header table. */
#define PT_STACK   0x6474e551   /**< Stack segment. */

/** Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /**< Executable. */
#define PF_W 2          /**< Writable. */
#define PF_R 4          /**< Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/** Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory and supplemental page table. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
#ifdef VM
  t->sup_page_table = vm_spt_create ();
  if (t->sup_page_table == NULL)
    goto done;
#endif
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
  t->execute_file = file;
  file_deny_write (t->execute_file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/** load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/** Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/** Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

#ifdef VM
      /* Lazy load */
      struct thread *cur = thread_current ();
      ASSERT (pagedir_get_page (cur->pagedir, upage) == NULL);

      if (!vm_spt_install_filesys (cur->sup_page_table, upage,
                                   file, ofs, page_read_bytes, page_zero_bytes, writable))
        return false;
#else
      /* Get a page of memory. */
      uint8_t *kpage = vm_frametable_allocate (PAL_USER, upage);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          vm_frametable_free (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          vm_frametable_free (kpage);
          return false;
        }
#endif

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
#ifdef VM
      ofs += PGSIZE;
#endif

    }

  return true;
}

/** Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = vm_frametable_allocate (PAL_USER | PAL_ZERO, PHYS_BASE - PGSIZE);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        vm_frametable_free (kpage);
    }
  return success;
}

/** Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  bool success = (pagedir_get_page (t->pagedir, upage) == NULL
                  && pagedir_set_page (t->pagedir, upage, kpage, writable));
#ifdef VM
  success = success && vm_spt_install_frame (t->sup_page_table, upage, kpage);
  if (success)
    vm_frametable_unpin (kpage);
#endif
  return success;
}
