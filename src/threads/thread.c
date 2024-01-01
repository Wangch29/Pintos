#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/** Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/** The maximum depth of nested priority donation. */
#define NESTED_DONATE_DEPTH 8

/** List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/** List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/** Idle thread. */
static struct thread *idle_thread;

/** Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/** Lock used by allocate_tid(). */
static struct lock tid_lock;

/** Load_avg in fix-point format(17.14). */
int32_t load_avg;

/** Scheduler should be working. */
static bool schedule_started = false;

/** Stack frame for kernel_thread(). */
struct kernel_thread_frame
  {
    void *eip;                  /**< Return address. */
    thread_func *function;      /**< Function to call. */
    void *aux;                  /**< Auxiliary data for function. */
  };

/** Statistics. */
static long long idle_ticks;    /**< # of timer ticks spent idle. */
static long long kernel_ticks;  /**< # of timer ticks in kernel threads. */
static long long user_ticks;    /**< # of timer ticks in user programs. */

/** Scheduling. */
#define TIME_SLICE 6            /**< # of timer ticks to give each thread. */
static unsigned thread_ticks;   /**< # of timer ticks since last yield. */

/** If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

static void mlfqs_update_priority (struct thread *t, void *aux UNUSED);
static void mlfqs_update_recent_cpu (struct thread *t, void *aux UNUSED);

/** Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/** Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void)
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();
  schedule_started = true;

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/** Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    {
      user_ticks++;
      t->recent_cpu += itof(1);
    }
#endif
  else
    kernel_ticks++;

  /* Check MLFQS mode. */
  if (thread_mlfqs)
  {
    /* Update recent_cpu of current thread. */
    if (t != idle_thread)
      t->recent_cpu += itof(1);

    int64_t ticks = timer_ticks();
    /* Once every fourth tick, update priority for every thread. */
    if (ticks % 4 == 0)
      thread_foreach (&mlfqs_update_priority, NULL);
    /* Once per second, update load_avg and recent_cpu. */
    if (ticks % TIMER_FREQ == 0)
    {
      /* Update load_avg. */
      mlfqs_update_load_avg ();
      /* Update recent_cpu time for every thread. */
      thread_foreach (&mlfqs_update_recent_cpu, NULL);
    }
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/** Prints thread statistics. */
void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/** Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
  struct thread *t;
  struct thread *current;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  current = thread_current ();

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Initialize process.
#ifdef USERPROG
  struct process *p = malloc (sizeof (struct process));
  if (p == NULL)
    return TID_ERROR;

  t->process = p;
  p->thread = t;
  p->pid = PID_INITIALIZING;
  p->exit = false;
  p->parent_sleeping = false;
  p->parent = current;
  sema_init (&p->sema, 0);
  list_push_back (&current->children_list, &p->elem);
#endif*/

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  /* Yield, when new created thread has higher priority
    than current threads */
  if (priority > current->priority)
    thread_yield ();

  return tid;
}

/** Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/** Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, &thread_compare_priority, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/** Returns the name of the running thread. */
const char *
thread_name (void)
{
  return thread_current ()->name;
}

/** Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/** Returns the running thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

/** Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
  ASSERT (!intr_context ());

  struct thread *cur = thread_current ();

#ifdef USERPROG
  process_exit ();
#endif

  /* Release all locks */
  for (struct list_elem *e = list_begin (&cur->hold_lock_list);
       e != list_end (&cur->hold_lock_list); e = list_next (e))
    {
      struct lock *lock = list_entry(e, struct lock, elem);
      lock_release(lock);
    }

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/** Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  /* Start working only when schedule_started is true. */
  if (!schedule_started)
    return;

  old_level = intr_disable ();
  if (cur != idle_thread)
    list_insert_ordered (&ready_list, &cur->elem,
                         &thread_compare_priority, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/** Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/** Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority)
{
  /* In mlfqs mode, ignore thread_set_priority. */
  if (thread_mlfqs)
    return;

  ASSERT (PRI_MIN <= new_priority && new_priority <= PRI_MAX);

  int old_priority = thread_get_priority();
  struct thread *current = thread_current ();

  current->base_priority = new_priority;

  /* If there is no higher priority donator or new priority is
     higher than current priority, set the priority too. */
  if (list_empty(&thread_current ()->hold_lock_list) ||
      new_priority > thread_current ()->priority)
    thread_current ()->priority = new_priority;

  /* When lowering priority, if new_priority is not the highest,
    yield immediately. */
  if (old_priority > new_priority)
    thread_yield();
}

/** Returns the current thread's priority. */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/** Compare the priority between two threads.
 *
 * @param e1 - the list_elem that the first thread contains.
 * @param e2 - the list_elem that the second thread contains.
 * @return 1 if thread1 have higher priority; 0 otherwise.
 */
bool
thread_compare_priority (const struct list_elem *e1,
                         const struct list_elem *e2,
                         void *aux UNUSED)
{
  return list_entry(e1, struct thread, elem)->priority >
          list_entry(e2, struct thread, elem)->priority;
}

/** Donates priority to thread.
    Return if donation succeed or not.

    If thread has already been in ready queue, reinsert it. */
bool thread_donate_priority (struct thread *t, int priority)
{
  /* Donate priority. */
  t->priority = priority;

  /* If thread is in ready queue, rearrange the place. */
  if (t->status == THREAD_READY)
    {
      for (struct list_elem *e = list_begin (&ready_list);
           e != list_end (&ready_list); e = list_next (e))
      {
        if (e == &t->elem)
        {
          /* Find the thread, remove it from ready queue. */
          list_remove (e);
          /* Insert the thread back to ready queue. */
          list_insert_ordered (&ready_list, &t->elem,
                               &thread_compare_priority, NULL);
          break;
        }
      }
    }
  return 1;
}

/** Donates priority to thread nested.
    Return if donation succeed or not. */
bool thread_donate_priority_nested (struct thread *t, int priority)
{
  struct thread *thread = t;
  bool success = 0;
  int depth = 0;

  while (depth < NESTED_DONATE_DEPTH)
    {
      if (priority < thread->priority)
        break;
      success = thread_donate_priority (thread, priority);
      /* If there is no waiting_lock, break. */
      if (thread->waiting_lock == NULL)
        break;
      /* Update lock->max_priority. */
      thread->waiting_lock->max_priority = priority;
      /* Set thread to the next depth level. */
      if ((thread = thread->waiting_lock->holder) == NULL)
        break;
      /* Update nested depth. */
      depth++;
    }
  return success;
}


/** Set thread *t's priority to the highest value among holding locks'
    max_priority and its base_priority. */
void thread_priority_recall (struct thread *t)
{
  if (thread_mlfqs)
    return;

  int new_priority = t->base_priority;

  /* Compare the largest priority in hold_lock_list to base_priority. */
  if (!list_empty(&t->hold_lock_list))
    {
      for (struct list_elem *e = list_begin(&t->hold_lock_list);
            e != list_end(&t->hold_lock_list); e = list_next(e))
        {
          struct lock *l = list_entry(e, struct lock, elem);
          new_priority = l->max_priority > new_priority ?
                         l->max_priority : new_priority;
        }
    }

  /* Recall priority. */
  t->priority = new_priority;

  /* If thread is in ready queue, rearrange the place. */
  if (t->status == THREAD_READY)
    {
      for (struct list_elem *e = list_begin (&ready_list);
           e != list_end (&ready_list); e = list_next (e))
      {
        if (e == &t->elem)
        {
          /* Find the thread, remove it from ready queue. */
          list_remove (e);
          /* Insert the thread back to ready queue. */
          list_insert_ordered (&ready_list, &t->elem,
                               &thread_compare_priority, NULL);
          break;
        }
      }
    }
}

/** Update one thread's priority in MLFQS mode. */
static void
mlfqs_update_priority (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread)
    return;

  /* Rough priority. */
  int32_t rough_priority =  PRI_MAX - ftoi(t->recent_cpu / 4) - 2 * t->nice;

  if (rough_priority >= PRI_MAX)
    t->priority = PRI_MAX;
  else if (rough_priority <= PRI_MIN)
    t->priority = PRI_MIN;
  else
    t->priority = (uint8_t) rough_priority;
}

/** Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice)
{
  struct thread *current = thread_current ();
  current->nice = nice;
  /* Calculate priority. */
  mlfqs_update_priority (current, NULL);
}

/** Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

/** Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return ftoi_round (100 * load_avg);
}

/** Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return ftoi_round (100 * thread_current ()->recent_cpu);
}

/** Update one thread's recent_cpu in MLFQS mode. */
static void
mlfqs_update_recent_cpu (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread)
    return;

  t->recent_cpu = mul_ff(div_ff(2 * load_avg, 2 * load_avg + itof(1))
                            , t->recent_cpu) + itof(t->nice);
}

/** Update all thread's recent_cpu in MLFQS mode. */
void
mlfqs_update_all_recent_cpu (void)
{
  thread_foreach (&mlfqs_update_recent_cpu, NULL);
}

/** Update load_avg in MLFQS mode. */
void
mlfqs_update_load_avg (void)
{
  fixed_point coef = div_ff(itof(59), itof(60));
  int ready_thread_cnt = list_size(&ready_list);
  if (thread_current() != idle_thread) {
    ready_thread_cnt++;
  }
  load_avg = mul_ff(coef, load_avg) + itof(ready_thread_cnt) / 60;
}

/** Update all thread's priorities. */
void
mlfqs_update_all_priorities (void)
{
  thread_foreach (&mlfqs_update_priority, NULL);
}

/** Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/** Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /**< The scheduler runs with interrupts off. */
  function (aux);       /**< Execute the thread function. */
  thread_exit ();       /**< If function() returns, kill the thread. */
}

/** Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the current thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/** Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/** Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;

  //t->waiting_lock = NULL;
  t->exit_status = 0;

  /* When not in mlfqs mode, set priority and priority donation devices. */
  if (!thread_mlfqs)
    {
      list_init (&t->hold_lock_list);
      t->priority = priority;
      t->base_priority = priority;
    }
  else
  /* When in mlfqs mode, set nice value. */
    {
      /* Set nice value, recent_cpu of initial_thread and load_avg to 0. */
      if (t == initial_thread)
        {
          t->nice = 0;
          t->recent_cpu = 0;
          load_avg = 0;
          t->priority = PRI_MIN;
        }
      else
      /* For other threads, inherit nice value from parent. */
        {
          t->nice = thread_current ()->nice;
          t->recent_cpu = thread_current ()->recent_cpu;
          mlfqs_update_priority(t, NULL);
        }
    }

#ifdef USERPROG
  list_init (&t->file_descriptor_table);
  list_init (&t->children_list);
  sema_init (&t->wait_child_load, 0);
  t->process = NULL;
  t->execute_file = NULL;
  t->next_fd = 2;
#endif

  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/** Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/** Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/** Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/** Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/** Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/** Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
