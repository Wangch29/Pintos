#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
#include <user/syscall.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "vm/suppagetable.h"

/* Declare intr_frame, which is defined in "threads/interrupt.h". */
struct intr_frame;

/* Syscall entry. */
static void syscall_handler (struct intr_frame *);

/* Syscall implementation functions. */
static void sys_exit (struct intr_frame *);
static void sys_halt (struct intr_frame *);
static void sys_exec (struct intr_frame *);
static void sys_wait (struct intr_frame *);
static void sys_create (struct intr_frame *);
static void sys_remove (struct intr_frame *);
static void sys_open (struct intr_frame *);
static void sys_filesize (struct intr_frame *);
static void sys_read (struct intr_frame *);
static void sys_write (struct intr_frame *);
static void sys_seek (struct intr_frame *);
static void sys_tell (struct intr_frame *);
static void sys_close (struct intr_frame *);
static void sys_mmap (struct intr_frame *);
static void sys_munmap (struct intr_frame *);

/* File system. */
static void sys_chdir (struct intr_frame *);
static void sys_mkdir (struct intr_frame *);
static void sys_readdir (struct intr_frame *f);
static void sys_isdir (struct intr_frame *f);
static void sys_inumber (struct intr_frame *f);

/* Functions to support reading from and writing to user memory for system calls.*/
static void *read_user_ptr (void *user_ptr, size_t size);
static void *write_user_ptr (void *user_ptr, size_t size);
static char *read_user_str (char *user_ptr_);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);

/* Terminate with error status. */
static void terminate_withError (void);

/* Functions about file. */
static struct file *get_file (int);
static void free_descriptor_table (void);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/** Handle system calls. */
static void
syscall_handler (struct intr_frame *f UNUSED)
{
  uint32_t syscall_num = *(int *) read_user_ptr (f->esp, sizeof (int32_t));

  /* Save user stack pointer. */
  thread_current ()->user_esp = f->esp;

  switch (syscall_num)
    {
      case SYS_HALT:sys_halt (f);
      break;
      case SYS_EXIT:sys_exit (f);
      break;
      case SYS_EXEC:sys_exec (f);
      break;
      case SYS_WAIT:sys_wait (f);
      break;
      case SYS_CREATE:sys_create (f);
      break;
      case SYS_REMOVE:sys_remove (f);
      break;
      case SYS_OPEN:sys_open (f);
      break;
      case SYS_FILESIZE:sys_filesize (f);
      break;
      case SYS_READ:sys_read (f);
      break;
      case SYS_WRITE:sys_write (f);
      break;
      case SYS_SEEK:sys_seek (f);
      break;
      case SYS_TELL:sys_tell (f);
      break;
      case SYS_CLOSE:sys_close (f);
      break;
      case SYS_MMAP:sys_mmap (f);
      break;
      case SYS_MUNMAP:sys_munmap (f);
      break;
      case SYS_CHDIR:sys_chdir (f);
      break;
      case SYS_MKDIR:sys_mkdir (f);
      break;
      case SYS_READDIR:sys_readdir (f);
      break;
      case SYS_ISDIR:sys_isdir (f);
      break;
      case SYS_INUMBER:sys_inumber (f);
      break;

      default:printf ("Cannot find this system call!\n");
      thread_exit ();
    }
}

/** Terminates Pintos by calling shutdown_power_off(). */
static void
sys_halt (struct intr_frame *f UNUSED)
{
  shutdown_power_off ();
}

/** Terminates the current user program, returning status to the kernel. */
static void
sys_exit (struct intr_frame *f UNUSED)
{
  struct thread *cur = thread_current ();
  int32_t exit_status = *(int32_t *) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (int32_t));

  f->eax = exit_status;
  if (cur->process != NULL)
    cur->process->exit_status = exit_status;

  thread_exit ();
}

/** Runs the executable whose name is given in cmd_line, passing any given
    arguments, and returns the new process's program id (pid).
    If the program cannot load or run for any reason, must return pid -1. */
static void
sys_exec (struct intr_frame *f UNUSED)
{
  const char *cmd_line = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (char *));
  read_user_str (cmd_line);
  f->eax = (tid_t) process_execute (cmd_line);
}

/** Waits for a child process pid and retrieves the child's exit status. */
static void
sys_wait (struct intr_frame *f UNUSED)
{
  f->eax = process_wait (*(tid_t *) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (tid_t)));
}

/** Creates a new file called file initially initial_size bytes in size.
    Creating a new file does not open it.
    Put boolean value of success on f->eax. */
static void
sys_create (struct intr_frame *f)
{
  const char *file_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (uintptr_t));
  unsigned initial_size = *(unsigned *) read_user_ptr (
      f->esp + 2 * sizeof (uintptr_t), sizeof (unsigned *)
  );

  read_user_str (file_name);

  //lock_acquire (&filesys_lock);
  bool success = filesys_create (file_name, initial_size, false);
  //lock_release (&filesys_lock);
  f->eax = success;
}

/** Deletes the file called file.
    A file may be removed regardless of whether it is open or closed,
    and removing an open file does not close it.
    Put boolean value of success or not on f->eax. */
static void
sys_remove (struct intr_frame *f)
{
  const char *file_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (char **));

  //lock_acquire (&filesys_lock);
  f->eax = filesys_remove (file_name);
  //lock_release (&filesys_lock);
}

/** Opens the file called file.
    Put a non-negative "file descriptor" on f->eax, or -1 if the file could not be opened.*/
static void
sys_open (struct intr_frame *f)
{
  const char *file_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (uintptr_t));
  read_user_str (file_name);

  //lock_acquire (&filesys_lock);
  struct file *file = filesys_open (file_name);
  //lock_release (&filesys_lock);
  /* If there is no such a file. */
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }

  struct file_table_entry *fte = malloc (sizeof (struct file_table_entry));
  if (fte == NULL)
    {
      f->eax = -1;
      return;
    }
  fte->fd = thread_current ()->next_fd++;
  f->eax = fte->fd;
  fte->file = file;
  list_push_back (&thread_current ()->file_descriptor_table, &fte->elem);
}

/** Set f->eax to the size, in bytes, of the file open as fd.
    If fd doesn't exist, set f->eax to -1. */
static void
sys_filesize (struct intr_frame *f)
{
  int fd = *(int32_t *) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (int));

  struct file *file = get_file (fd);
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }
  //lock_acquire (&filesys_lock);
  f->eax = file_length (file);
  //lock_release (&filesys_lock);
}

/** Reads size bytes from the file open as fd into buffer.
    Returns the number of bytes actually read (0 at end of file),
    or -1 if the file could not be read (due to a condition other than end of file).
    Fd 0 reads from the keyboard using input_getc(). */
static void
sys_read (struct intr_frame *f)
{
  int fd = *(int32_t *) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (int));
  void *buffer = *(void **) read_user_ptr (f->esp + 2 * sizeof (uintptr_t), sizeof (void **));
  unsigned size = *(uint32_t *) read_user_ptr (f->esp + 3 * sizeof (uintptr_t), sizeof (unsigned));

  write_user_ptr (buffer, size);

  /* Read from STDIN. */
  if (fd == STDIN_FILENO)
    {
      for (int i = 0; i < size; i++)
        {
          *(char *) buffer = input_getc ();
          buffer += sizeof (char *);
        }
      f->eax = size;
      return;
    }

  /* Read from STDOUT. */
  if (fd == STDOUT_FILENO)
    terminate_withError ();

  /* Read from files. */
  struct file *file = get_file (fd);
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }

#ifdef VM
  load_pin_pages (buffer, size);
#endif
  //lock_acquire (&filesys_lock);
  f->eax = file_read (file, buffer, size);
  //lock_release (&filesys_lock);
#ifdef VM
  unpin_pages (buffer, size);
#endif
}

/** Writes size bytes from buffer to the open file fd.
 *  Set f->eax to the number of bytes actually written.
 *  Fd 1 writes to the console.
 *
 *  If fd represents a directory, set f->eax to -1.
*/
static void
sys_write (struct intr_frame *f UNUSED)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof (uintptr_t)), sizeof (uint32_t * ));
  void *buffer = read_user_ptr (*(void **) (f->esp + 2 * sizeof (uintptr_t)), sizeof (void **));
  uint32_t size = *(uint32_t *) read_user_ptr (f->esp + 3 * sizeof (uintptr_t), sizeof (uint32_t));

  read_user_ptr (buffer, size);

  /* Write to STDIN. */
  if (fd == STDIN_FILENO)
    terminate_withError ();

  /* Write to STDOUT. */
  if (fd == STDOUT_FILENO)
    {
      putbuf ((char *) buffer, size);
      f->eax = size;
      return;
    }

  /* Write to files. */
  struct file *file = get_file (fd);
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }

  //lock_acquire (&filesys_lock);

  /* Check if file is a directory. */
  if (inode_is_dir (file_get_inode (file)))
    {
      f->eax = -1;
      //lock_release (&filesys_lock);
      return;
    }

#ifdef VM
  load_pin_pages (buffer, size);
#endif
  f->eax = file_write (file, buffer, size);
  //lock_release (&filesys_lock);
#ifdef VM
  unpin_pages (buffer, size);
#endif
}

/** Changes the next byte to be read or written in open file fd to position,
    expressed in bytes from the beginning of the file. */
static void
sys_seek (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof (uintptr_t)), sizeof (uint32_t));
  uint32_t position = *(uint32_t *) read_user_ptr ((f->esp + 2 * sizeof (uintptr_t)), sizeof (uint32_t));

  struct file *file = get_file (fd);
  if (file == NULL)
    return;
  //lock_acquire (&filesys_lock);
  file_seek (file, position);
  //lock_release (&filesys_lock);
}

/** Returns the position of the next byte to be read or written in open file fd,
    expressed in bytes from the beginning of the file. */
static void
sys_tell (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof (uintptr_t)), sizeof (uint32_t * ));

  if (fd <= 1)
    {
      f->eax = -1;
      return;
    }
  struct file *file = get_file (fd);
  if (f == NULL)
    {
      f->eax = -1;
      return;
    }
  //lock_acquire (&filesys_lock);
  f->eax = file_tell (file);
  //lock_release (&filesys_lock);
}

/** Closes file descriptor fd. */
static void
sys_close (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof (uintptr_t)), sizeof (uint32_t * ));
  struct thread *current = thread_current ();

  if (fd <= 1)
    return;

  for (struct list_elem *e = list_begin (&current->file_descriptor_table);
       e != list_end (&current->file_descriptor_table); e = list_next (e))
    {
      struct file_table_entry *fte = list_entry (e,
      struct file_table_entry, elem);
      if (fte->fd == fd)
        {
          list_remove (e);
          //lock_acquire (&filesys_lock);
          file_close (fte->file);
          //lock_release (&filesys_lock);
          free (fte);
          return;
        }
    }
}

/** Changes the current working directory of the process to "dir_name". */
static void
sys_chdir (struct intr_frame *f)
{
  const char *dir_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (uintptr_t));
  read_user_str (dir_name);

  //lock_acquire (&filesys_lock);
  bool success = filesys_chdir (dir_name);
  //lock_release (&filesys_lock);

  f->eax = success;
  return;
}

/** Create a new directory. */
static void
sys_mkdir (struct intr_frame *f)
{
  const char *dir_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (uintptr_t));
  read_user_str (dir_name);

  //lock_acquire (&filesys_lock);
  bool success = filesys_create (dir_name, 0, true);
  //lock_release (&filesys_lock);

  f->eax = success;
  return;
}

/** Read a directory entry from a file descriptor, which must represent a directory.
 *
 * If succeed, stores the null-terminated file name in "dir_name". */
static void
sys_readdir (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof (uintptr_t)), sizeof (uintptr_t));
  char *dir_name = *(const char **) read_user_ptr (f->esp + 2 * sizeof (uintptr_t), sizeof (uintptr_t));
  write_user_ptr (dir_name, READDIR_MAX_LEN + 1);

  /* Get the struct file. */
  struct file *file = get_file (fd);
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }

  //lock_acquire (&filesys_lock);

  /* Check if it's a directory. */
  struct inode *inode = file_get_inode (file);
  if (!inode_is_dir (inode))
    {
      //lock_release (&filesys_lock);
      f->eax = -1;
      return;
    }

  struct dir *dir = (struct dir *) file;

  f->eax = dir_readdir (dir, dir_name);
  //lock_release (&filesys_lock);

  return;
}

/** Returns true if fd represents a directory, false if it represents an ordinary file. */
static void
sys_isdir (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof (uintptr_t)), sizeof (uintptr_t));
  struct file *file = get_file (fd);
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }

  //lock_acquire (&filesys_lock);
  f->eax = inode_is_dir (file_get_inode (file));
  //lock_release (&filesys_lock);

  return;
}

/** Returns the inode number of the inode associated with fd. */
static void
sys_inumber (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof (uintptr_t)), sizeof (uintptr_t));
  struct file *file = get_file (fd);
  if (file == NULL)
    terminate_withError ();

  //lock_acquire (&filesys_lock);
  f->eax = inode_get_inumber (file_get_inode (file));
  //lock_release (&filesys_lock);

  return;
}

#ifdef VM
/**
 * Handle mmap syscall.
 */
static void sys_mmap (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof(uintptr_t)), sizeof(uintptr_t));
  void *mapped_addr = *(void **) read_user_ptr ((f->esp + 2 * sizeof (uintptr_t)), sizeof (uintptr_t));

  /** Check if mapped_addr and fd is valid. */
  if (!is_user_vaddr (mapped_addr) || pg_ofs (mapped_addr) != 0 || mapped_addr == NULL || fd <= 1)
    {
      f->eax = -1;
      return;
    }

  //lock_acquire (&filesys_lock);

  struct thread* cur = thread_current ();

  /* Open file. */
  struct file *file = get_file (fd);
  if (file)
    file = file_reopen (file);
  if (file == NULL)
    goto SYS_MMAP_FAIL;

  size_t filesize = file_length (file);
  if(filesize == 0)
    goto SYS_MMAP_FAIL;

  /* Check if all mapped pages are emtpy. */
  for (size_t offset = 0; offset < filesize; offset += PGSIZE)
    {
      void *mapped_page = mapped_addr + offset;
      if (vm_spt_has_page (cur->sup_page_table, mapped_page))
        goto SYS_MMAP_FAIL;
    }

  /* Mapping file to pages. */
  for (size_t offset = 0; offset < filesize; offset += PGSIZE)
    {
      void *mapped_page = mapped_addr + offset;

      size_t read_bytes = offset + PGSIZE < filesize ? PGSIZE : filesize - offset;
      size_t zero_bytes = PGSIZE - read_bytes;

      if (!vm_spt_install_filesys
      (cur->sup_page_table, mapped_page, file, offset, read_bytes, zero_bytes, true))
        goto SYS_MMAP_FAIL;
    }

  /* Create new mmap_table_entry. */
  mapid_t new_id;
  if (list_empty (&cur->mmap_table))
    new_id = 1;
  else
    new_id = list_entry (list_back (&cur->mmap_table), struct mmap_table_entry, elem)->map_id + 1;

  struct mmap_table_entry *mte = (struct mmap_table_entry*) malloc (sizeof (struct mmap_table_entry));
  mte->map_id = new_id;
  mte->upage = mapped_addr;
  mte->file = file;
  mte->size = filesize;
  list_push_back (&cur->mmap_table, &mte->elem);

  /* Mapping succeeds. */
  //lock_release (&filesys_lock);
  f->eax = new_id;
  return;

SYS_MMAP_FAIL:
  //lock_release (&filesys_lock);
  f->eax = -1;
  return;
}

/**
 * Handle munmap syscall.
 */
static void sys_munmap (struct intr_frame *f)
{
  uint32_t map_id = *(uint32_t *) read_user_ptr ((f->esp + sizeof(uintptr_t)), sizeof(uintptr_t));

  struct thread* cur = thread_current ();

  /* Search for the map_id's mmap_table_entry. */
  for (struct list_elem *e = list_begin (&cur->mmap_table);
       e!= list_end (&cur->mmap_table); e = list_next (e))
    {
      struct mmap_table_entry *mte = list_entry (e, struct mmap_table_entry, elem);

      if (mte->map_id == map_id)
      /* Found the map_id, and start to unmap. */
        {
          //lock_acquire (&filesys_lock);

          size_t filesize = file_length (mte->file);
          for (size_t offset = 0; offset < filesize; offset += PGSIZE)
            {
              void *addr = mte->upage + offset;
              size_t bytes = (offset + PGSIZE < filesize ? PGSIZE : filesize - offset);
              vm_spt_mm_unmap (cur->sup_page_table, cur->pagedir, addr, mte->file, offset, bytes);
            }

          list_remove (e);
          file_close (mte->file);
          free (mte);

          //lock_release (&filesys_lock);
          return;
        }
    }

  /* Cannot find the map_id. */
  f->eax = -1;
  return;
}
#endif

#ifdef VM
/** Load and pin all the pages in buffer. */
void
load_pin_pages (const void *buffer, size_t size)
{
  struct sup_page_table *spt = thread_current ()->sup_page_table;

  uint32_t *pagedir = thread_current ()->pagedir;

  for (void *upage = pg_round_down (buffer); upage < buffer + size; upage += PGSIZE)
    {
      vm_load_page (spt, pagedir, upage);
      vm_spt_pin (spt, upage);
    }
}

/** Unpin all pages in buffer. */
void
unpin_pages(const void *buffer, size_t size)
{

  struct sup_page_table *spt = thread_current ()->sup_page_table;

  for (void *upage = pg_round_down (buffer); upage < buffer + size; upage += PGSIZE)
    vm_spt_unpin (spt, upage);
}
#endif

/** Attempt to read a pointer of size bytes from the user.
    If user_ptr is above PHYS_BASE or cannot be accessed,
    terminate this process. */
static void *
read_user_ptr (void *user_ptr, size_t size)
{
  /* If user_ptr is not user_vaddr, exit with error. */
  if (!is_user_vaddr (user_ptr))
    terminate_withError ();

  for (size_t i = 0; i < size; i++)
    {
      if (get_user ((uint8_t * ) (user_ptr + i)) == -1)
        terminate_withError ();
    }
  return (void *) user_ptr;
}

/** Attempting to write a pointer of size bytes to the user. */
static void *
write_user_ptr (void *user_ptr, size_t size)
{
  /* If user_ptr is not user_vaddr, exit with error. */
  if (!is_user_vaddr (user_ptr))
    terminate_withError ();

  for (size_t i = 0; i < size; i++)
    {
      if (!put_user ((uint8_t *) user_ptr + i, 0))
        terminate_withError ();
    }
  return (void *) user_ptr;
}

/** Attempt to read a pointer of string from the user. */
static char *
read_user_str (char *user_ptr_)
{
  /* If user_ptr is not user_vaddr, exit with error. */
  if (!is_user_vaddr (user_ptr_))
    terminate_withError ();

  uint8_t *user_ptr = (uint8_t *) user_ptr_;

  while (1)
    {
      char c = get_user (user_ptr);
      if (c == -1)
        terminate_withError ();
      if (c == '\0')
        return user_ptr_;
      user_ptr += 1;
    }
}

/** Terminate current thread with exit_status -1. */
static void
terminate_withError (void)
{
  struct thread *cur = thread_current ();
  if (cur != NULL)
    cur->process->exit_status = -1;
  thread_exit ();
}

/** Return the file pointer by fd from file_descriptor_table.
    If fd doesn't exist, return NULL. */
static struct file *
get_file (int fd)
{
  if (fd <= 1)
    return NULL;

  struct thread *current = thread_current ();

  if (fd >= current->next_fd)
    return NULL;

  for (struct list_elem *e = list_begin (&current->file_descriptor_table);
       e != list_end (&current->file_descriptor_table); e = list_next (e))
    {
      struct file_table_entry *fte = list_entry (e,
      struct file_table_entry, elem);
      if (fte->fd == fd)
        return fte->file;
    }
  return NULL;
}

/** Reads a byte at user virtual address UADDR.
    UADDR must be below PHYS_BASE.
    Returns the byte value if successful, -1 if a segfault
    occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a" (result) : "m" (*uaddr));
  return result;
}

/** Writes BYTE to user address UDST.
    UDST must be below PHYS_BASE.
    Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}