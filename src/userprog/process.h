#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

#define PID_ERROR ((pid_t) -1)
#define PID_INITIALIZING ((pid_t) -2)

typedef int pid_t;
typedef int32_t mapid_t;

struct process;
struct file_table_entry;

/** User process. */
struct process
{
    pid_t pid;                              /**< Process id.                           */
    char *cmd;                              /**< Process command.                      */
    struct thread *thread;                  /**< Thread.                               */
    struct thread *parent;                  /**< Parent thread.                        */

    bool exit;                              /**< Exit or not.                          */
    bool parent_sleeping;                   /**< Parent is sleeping or not.            */
    int32_t exit_status;                    /**< Exit status.                          */

    struct semaphore sema;                  /**< Semaphore that parent sleeps on.      */

    struct list_elem elem;                  /**< List_elem for parent's children_list. */
};

/** File table */
struct file_table_entry
  {
      uint32_t fd;               /**< File descriptor. */
      struct file *file;         /**< File pointer.    */
      struct list_elem elem;     /**< List_elem.       */
  };

/** Memory-mapped table entry. */
struct mmap_table_entry
  {
    mapid_t map_id;              /**< Map ID.             */
    void *upage;                 /**< The upage mapped to.*/
    struct file *file;           /**< File pointer.       */
    size_t size;                 /**< File size.          */
    struct list_elem elem;       /**< List_elem.          */
  };

pid_t process_execute (const char *process_args);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /**< userprog/process.h */
