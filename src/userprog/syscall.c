#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

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

/* Functions to support reading from and writing to user memory for system calls.*/
static void* read_user_ptr (void *user_ptr, size_t size);
static void* write_user_ptr (void *user_ptr, size_t size);
static char* read_user_str (char* user_ptr_);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);

/* Terminate with error status. */
static void terminate_withError (void);

/* Functions about file. */
static struct file* get_file (int);
static void free_descriptor_table (void);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/** Handle system calls.   */
static void
syscall_handler (struct intr_frame *f UNUSED)
{
  uint32_t syscall_num = *(int *) read_user_ptr(f->esp, sizeof(int32_t));

  switch (syscall_num)
  {
    case SYS_HALT:
      sys_halt (f);
      break;
    case SYS_EXIT:
      sys_exit (f);
      break;
    case SYS_EXEC:
      sys_exec (f);
      break;
    case SYS_WAIT:
      sys_wait (f);
      break;
    case SYS_CREATE:
      sys_create (f);
      break;
    case SYS_REMOVE:
      sys_remove (f);
      break;
    case SYS_OPEN:
      sys_open (f);
      break;
    case SYS_FILESIZE:
      sys_filesize (f);
      break;
    case SYS_READ:
      sys_read (f);
      break;
    case SYS_WRITE:
      sys_write (f);
      break;
    case SYS_SEEK:
      sys_seek (f);
      break;
    case SYS_TELL:
      sys_tell (f);
      break;
    case SYS_CLOSE:
      sys_close (f);
      break;
    default:
      printf ("Cannot find this system call!\n");
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
  int32_t exit_status_ = *(int32_t *) read_user_ptr(f->esp + sizeof(uintptr_t), sizeof (int32_t));

  f->eax = exit_status_;
  thread_current ()->exit_status = exit_status_;

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
  f->eax = process_wait (*(tid_t *) read_user_ptr (f->esp + sizeof(uintptr_t), sizeof (tid_t)));
}

/** Creates a new file called file initially initial_size bytes in size.
    Creating a new file does not open it.
    Put boolean value of success on f->eax. */
static void
sys_create (struct intr_frame *f)
{
  const char* file_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (uintptr_t));
  unsigned initial_size = *(unsigned *) read_user_ptr (
          f->esp + 2 * sizeof (uintptr_t),sizeof (unsigned *)
          );

  read_user_str(file_name);

  lock_acquire (&filesys_lock);
  bool success = filesys_create (file_name, initial_size);
  lock_release (&filesys_lock);
  f->eax = success;
}

/** Deletes the file called file.
    A file may be removed regardless of whether it is open or closed,
    and removing an open file does not close it.
    Put boolean value of success or not on f->eax. */
static void
sys_remove (struct intr_frame *f)
{
  const char* file_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (char **));

  lock_acquire (&filesys_lock);
  f->eax = filesys_remove (file_name);
  lock_release (&filesys_lock);
}

/** Opens the file called file.
    Put a non-negative "file descriptor" on f->eax, or -1 if the file could not be opened.*/
static void
sys_open (struct intr_frame *f)
{
  const char* file_name = *(const char **) read_user_ptr (f->esp + sizeof (uintptr_t), sizeof (uintptr_t));
  read_user_str(file_name);

  lock_acquire (&filesys_lock);
  struct file *file = filesys_open (file_name);
  lock_release (&filesys_lock);
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
  int fd = *(int32_t *) read_user_ptr (f->esp + sizeof(uintptr_t), sizeof (int));

  struct file *file = get_file(fd);
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }
  lock_acquire (&filesys_lock);
  f->eax = file_length (file);
  lock_release (&filesys_lock);
}

/** Reads size bytes from the file open as fd into buffer.
    Returns the number of bytes actually read (0 at end of file),
    or -1 if the file could not be read (due to a condition other than end of file).
    Fd 0 reads from the keyboard using input_getc(). */
static void sys_read (struct intr_frame *f)
{
  int fd = *(int32_t *) read_user_ptr (f->esp + sizeof(uintptr_t), sizeof (int));
  void *buffer = *(void **) read_user_ptr(f->esp + 2 * sizeof(uintptr_t), sizeof (void **));
  unsigned size = *(uint32_t *) read_user_ptr (f->esp + 3 * sizeof(uintptr_t), sizeof (unsigned));

  write_user_ptr (buffer, size);

  /* Read from STDIN. */
  if (fd == STDIN_FILENO)
  {
    for (int i = 0; i < size; i++)
      {
        *(char *) buffer = input_getc();
        buffer += sizeof (char *);
      }
    f->eax = size;
    return;
  }
  /* Read from STDOUT. */
  if (fd == STDOUT_FILENO)
    terminate_withError ();
  /* Read from files. */
  struct file *file = get_file(fd);
  if (file == NULL)
    {
      f->eax = -1;
      return;
    }
  lock_acquire (&filesys_lock);
  f->eax = file_read (file, buffer, size);
  lock_release (&filesys_lock);
}

/** Writes size bytes from buffer to the open file fd.
    Set f->eax to the number of bytes actually written.
    Fd 1 writes to the console. */
static void
sys_write (struct intr_frame *f UNUSED)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof(uintptr_t)), sizeof (uint32_t *));
  void *buffer = read_user_ptr (*(void **) (f->esp + 2 * sizeof (uintptr_t)), sizeof (void **));
  uint32_t size = *(uint32_t *) read_user_ptr(f->esp + 3 * sizeof(uintptr_t), sizeof (uint32_t));

  read_user_ptr (buffer, size);

  /* Write to STDIN. */
  if (fd == STDIN_FILENO)
    terminate_withError();
  /* Write to STDOUT. */
  if (fd == STDOUT_FILENO)
    {
      putbuf ((char *) buffer, size);
      f->eax = size;
      return;
    }
  /* Write to files. */
  struct file *file = get_file(fd);
  if (file == NULL)
  {
    f->eax = -1;
    return;
  }
  lock_acquire (&filesys_lock);
  f->eax = file_write (file, buffer, size);
  lock_release (&filesys_lock);
}

/** Changes the next byte to be read or written in open file fd to position,
    expressed in bytes from the beginning of the file. */
static void
sys_seek (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof(uintptr_t)), sizeof (uint32_t));
  uint32_t position = *(uint32_t *) read_user_ptr ((f->esp + 2 * sizeof(uintptr_t)), sizeof (uint32_t));

  struct file *file = get_file(fd);
  if (file == NULL)
    return;
  lock_acquire (&filesys_lock);
  file_seek (file, position);
  lock_release (&filesys_lock);
}

/** Returns the position of the next byte to be read or written in open file fd,
    expressed in bytes from the beginning of the file. */
static void
sys_tell (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof(uintptr_t)), sizeof (uint32_t *));

  if (fd <= 1)
    {
      f->eax = -1;
      return;
    }
  struct file *file = get_file(fd);
  if (f == NULL)
    {
      f->eax = -1;
      return;
    }
  lock_acquire (&filesys_lock);
  f->eax = file_tell (file);
  lock_release (&filesys_lock);
}

/** Closes file descriptor fd. */
static void
sys_close (struct intr_frame *f)
{
  uint32_t fd = *(uint32_t *) read_user_ptr ((f->esp + sizeof(uintptr_t)), sizeof (uint32_t *));
  struct thread *current = thread_current ();

  if (fd <= 1)
    return;

  for (struct list_elem *e = list_begin (&current->file_descriptor_table);
       e != list_end (&current->file_descriptor_table); e = list_next (e))
    {
      struct file_table_entry *fte = list_entry (e, struct file_table_entry, elem);
      if (fte->fd == fd)
        {
          list_remove (e);
          lock_acquire (&filesys_lock);
          file_close (fte->file);
          lock_release (&filesys_lock);
          free (fte);
          return;
        }
    }
}

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
      if (get_user((uint8_t *) (user_ptr + i)) == -1)
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
static char*
read_user_str (char* user_ptr_)
{
  /* If user_ptr is not user_vaddr, exit with error. */
  if (!is_user_vaddr (user_ptr_))
    terminate_withError ();

  uint8_t* user_ptr = (uint8_t *) user_ptr_;

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
  thread_current ()->exit_status = -1;
  thread_exit ();
}

/** Return the file pointer by fd from file_descriptor_table.
    If fd doesn't exist, return NULL. */
static struct file*
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
      struct file_table_entry *fte = list_entry (e, struct file_table_entry, elem);
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