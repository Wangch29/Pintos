#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

typedef int tid_t;

tid_t process_execute (const char *process_args);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /**< userprog/process.h */
