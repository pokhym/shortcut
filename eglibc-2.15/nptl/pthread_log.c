#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "pthreadP.h"
#include "pthread_log.h"
#include "semaphore.h"
#include <fcntl.h>

#include <shlib-compat.h>

// Turns debugging on and off
//#define DPRINT pthread_log_debug
#define DPRINT(x,...)

// Globals for user-level replay
int pthread_log_status = PTHREAD_LOG_NONE;
unsigned long* ppthread_log_clock = 0;

// System calls we added
#define __NR_pthread_print 	17
#define __NR_pthread_log        31
#define __NR_pthread_block      32
#define __NR_pthread_init       35
#define __NR_pthread_full       44
#define __NR_pthread_sysign     53
#define __NR_pthread_status     56
#define __NR_pthread_get_clock  58
#define __NR_pthread_shm_path   98

// This prints a message outside of the record/replay mechanism - useful for debugging
void pthread_log_debug(const char* fmt,...)
{
    va_list ap;
    char buffer[256] = {0,};
    INTERNAL_SYSCALL_DECL(__err);
    va_start(ap,fmt);
    vsprintf(buffer,fmt,ap);
    INTERNAL_SYSCALL(pthread_print,__err,2,buffer,256);
    va_end(ap);
}

void pthread_log_stat (void)
{
    INTERNAL_SYSCALL_DECL(__err);
    INTERNAL_SYSCALL(pthread_status,__err,1,&pthread_log_status);
}

// Kernel will set the recplay status.  Also tells the kernel where the replay clock lives
static void pthread_log_init (void)
{
    void* ppage;
    int page_exists;

    // see if the kernel already put the replay clock in the user's address space
    INTERNAL_SYSCALL_DECL(__err);
    page_exists = INTERNAL_SYSCALL(pthread_get_clock,__err,1,&ppage);
    
    if (page_exists) {
        // we'll just set the clock to the one that the kernel set up for us
        ppthread_log_clock = (unsigned long *) ppage;
	DPRINT ("the page exists and was set by the kernel %p\n", ppage);
    } else {
        // else, the kernel never set up our page
        int fd;
        INTERNAL_SYSCALL_DECL(__err);
        fd = INTERNAL_SYSCALL(pthread_shm_path,__err,0);
	if (fd < 0) {
	    printf("shm_open failed\n");
	    exit (0);
	}

        ppage = mmap (0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

        DPRINT ("Initial mmap returns %p\n", ppage);
        if (ppage == MAP_FAILED) {
            printf ("Cannot setup shared page for clock\n");
            exit (0);
        }

        ppthread_log_clock = (unsigned long *) ppage;

        INTERNAL_SYSCALL_DECL(__err);
        INTERNAL_SYSCALL(pthread_init,__err,4,&pthread_log_status,ppthread_log_clock,(u_long)pthread_log_record,(u_long)pthread_log_replay);
    }
}

// This informs the kernel about a newly allocated log
void pthread_log_alloc (struct pthread_log_head * log_addr, int* ignore_addr)
{
    INTERNAL_SYSCALL_DECL(__err);
    INTERNAL_SYSCALL(pthread_log,__err,2,log_addr,ignore_addr);
}

// This informs the kernel about a thread blocking on user clock - returns when thread is unblocked
void pthread_log_block (u_long clock)
{
    INTERNAL_SYSCALL_DECL(__err);
    INTERNAL_SYSCALL(pthread_block,__err,1,clock);
}

// This informs the kernel that the log is full
void pthread_log_full (void)
{
    INTERNAL_SYSCALL_DECL(__err);
    INTERNAL_SYSCALL(pthread_full,__err,0);
}

// Fake syscall for signals delivered in ignored region
void pthread_sysign (void)
{
    INTERNAL_SYSCALL_DECL(__err);
    INTERNAL_SYSCALL(pthread_sysign,__err,0);
}

#include <bits/libc-lock.h>

void pthread_log_mutex_lock (__libc_lock_t* lock)
{
    if (is_recording()) { 
        pthread_log_record (0, LIBC_LOCK_LOCK_ENTER, (u_long) lock, 1); 
	__libc_lock_lock(*(lock));
        pthread_log_record (0, LIBC_LOCK_LOCK_EXIT, (u_long) lock, 0); 
    } else if (is_replaying()) {
        pthread_log_replay (LIBC_LOCK_LOCK_ENTER, (u_long) lock); 
        pthread_log_replay (LIBC_LOCK_LOCK_EXIT, (u_long) lock); 
    } else {
	__libc_lock_lock(*(lock));
    }
}

int pthread_log_mutex_trylock (__libc_lock_t* lock)
{
    int ret;		  
    if (is_recording()) { 
        pthread_log_record (0, LIBC_LOCK_TRYLOCK_ENTER, (u_long) lock, 1); 
        ret = __libc_lock_trylock(*(lock)); 
        pthread_log_record (ret, LIBC_LOCK_TRYLOCK_EXIT, (u_long) lock, 0); 
    } else if (is_replaying()) { 
	pthread_log_replay (LIBC_LOCK_TRYLOCK_ENTER, (u_long) lock); 
        ret = pthread_log_replay (LIBC_LOCK_TRYLOCK_EXIT, (u_long) lock); 
    } else {
        ret = __libc_lock_trylock(*(lock)); 
    }								   
    return ret;							   
}

void pthread_log_mutex_unlock (__libc_lock_t* lock)
{
    if (is_recording()) { 
        pthread_log_record (0, LIBC_LOCK_UNLOCK_ENTER, (u_long) lock, 1); 
        __libc_lock_unlock (*(lock));
        pthread_log_record (0, LIBC_LOCK_UNLOCK_EXIT, (u_long) lock, 0); 
    } else if (is_replaying()) {
        pthread_log_replay (LIBC_LOCK_UNLOCK_ENTER, (u_long) lock); 
        pthread_log_replay (LIBC_LOCK_UNLOCK_EXIT, (u_long) lock); 
    } else {
	__libc_lock_unlock (*(lock));
    }
}

void (*pthread_log_lock)(int *);
int (*pthread_log_trylock)(int *);
void (*pthread_log_unlock)(int *);

extern void malloc_setup (void (*__pthread_log_lock)(int *),
			  int (*__pthread_log_trylock)(int *),
			  void (*__pthread_log_unlock)(int *));

int check_recording (void) 
{
    struct pthread_log_head* head;

    if (pthread_log_status == PTHREAD_LOG_REP_AFTER_FORK) {
	// After a fork, we need to reload the log 
	head = THREAD_GETMEM (THREAD_SELF, log_head);
	pthread_log_full ();
#ifdef USE_DEBUG_LOG	
	head->next = (struct pthread_log_data *) ((char *) head + sizeof (struct pthread_log_head));
#else
	head->next = ((char *) head + sizeof (struct pthread_log_head));
	head->expected_clock = 0;
	head->num_expected_records = 0;
#endif
	head->ignore_flag = 0;
	head->need_fake_calls = 0;
	DPRINT ("check_recording: head %p head->next %p\n", head, head->next);
	pthread_log_status = PTHREAD_LOG_REPLAY;
	return pthread_log_status;
    }

    // mcc: we need to get the status before allocating a log, so I've separated out
    // the status check with the log init. (This adds the cost of one more syscall on init)
    pthread_log_stat();

    if (pthread_log_status == PTHREAD_LOG_OFF)
	return PTHREAD_LOG_OFF;


    head = allocate_log();
    THREAD_SETMEM (THREAD_SELF, log_head, head);
    DPRINT ("Allocated log for main thread\n");
    pthread_log_alloc (head, &head->ignore_flag);
    head->ignore_flag = 1;
    pthread_log_init ();
    head->ignore_flag = 0;
    DPRINT ("Kernel sets log status to %d\n", pthread_log_status);
    malloc_setup(pthread_log_mutex_lock, pthread_log_mutex_trylock, pthread_log_mutex_unlock);
    DPRINT("end of check recording, head %p, pthread_log_status %d, ppthread_log_clock %p, *ppthread_log_clock %lu\n", head, pthread_log_status, ppthread_log_clock, *ppthread_log_clock);

    return pthread_log_status;
}

struct pthread_log_head *
allocate_log (void)
{
    struct pthread_log_head* head;
    u_long size = sizeof(struct pthread_log_head) + PTHREAD_LOG_SIZE;
    if (size % 4096) size += 4096 - (size%4096);
    head = (struct pthread_log_head *) mmap (0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (head == NULL) {
	pthread_log_debug ("Unable to allocate thread log\n");
	abort ();
    }
    head->ignore_flag = 0;
    head->need_fake_calls = 0;
#ifdef USE_DEBUG_LOG
    head->next = (struct pthread_log_data *) ((char *) head + sizeof (struct pthread_log_head));
    head->end = (struct pthread_log_data *) ((char *) head + sizeof (struct pthread_log_head) + PTHREAD_LOG_SIZE);
#else
    head->next = ((char *) head + sizeof (struct pthread_log_head));
    head->end = ((char *) head + sizeof (struct pthread_log_head) + PTHREAD_LOG_SIZE);
    head->expected_clock = 0;
    head->num_expected_records = 0;
#endif

    // Note that there is allocated space left after end - we need this to handle variable-sized records
    return head;
}

void
register_log (void)
{
    struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);
    pthread_log_alloc (head, &head->ignore_flag);
    DPRINT ("Registered ignore flag at %p value %d\n", &head->ignore_flag, head->ignore_flag);
}

void
free_log (void)
{
    DPRINT ("Freed (not really - fixme) thread log\n");
}

#ifdef USE_DEBUG_LOG
void
pthread_log_record (int retval, unsigned long type, unsigned long check, int is_entry)
{
    struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);
    struct pthread_log_data* data;
    int num_signals;

    if (head == NULL) return;
    if (head->need_fake_calls > 0) {
	// Some signals were delivered while we were ignoring syscalls
	// Add a record via recursive call to generate fake syscalls on replay
	num_signals = head->need_fake_calls;
	head->need_fake_calls = 0;

	data = head->next;
	data->retval = num_signals;
	data->type = FAKE_SYSCALLS;
	DPRINT ("Added fake record to log: num_signals %d\n", num_signals);
	(head->next)++; // Increment to next log record
	if (head->next == head->end) {
	    pthread_log_full();
	    head->next = (struct pthread_log_data *) ((char *) head + sizeof (struct pthread_log_head));
	}
    }
    data = head->next;
    data->clock = atomic_exchange_and_add (ppthread_log_clock, 1);
    data->retval = retval;
    data->type = type;
    data->check = check;

    head->ignore_flag = is_entry;
    DPRINT ("Added record to log: clock %lu retval %d type %lu check %lx\n", data->clock, data->retval, data->type, data->check);
    (head->next)++; // Increment to next log record
    if (head->next == head->end) {
	pthread_log_full();
	head->next = (struct pthread_log_data *) ((char *) head + sizeof (struct pthread_log_head));
    }
}

int 
pthread_log_replay (unsigned long type, unsigned long check)
{
    struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);
    struct pthread_log_data* data;
    int i;

    if (head == NULL) return 0;
    data = head->next;

    DPRINT ("Read record from log: clock %lu, retval %d, type %lu, check %lx\n", data->clock, data->retval, data->type, data->check);

    if (data->type == 0) { // We've reached the end of the log - some other thread may be able to run, though
	pthread_log_block (INT_MAX); 
    }
    while (data->type == FAKE_SYSCALLS) {
	(head->next)++; // Increment to next log record
	if (head->next == head->end) {
	    DPRINT ("Log is full - need to reload\n");
	    pthread_log_full ();
	    head->next = (struct pthread_log_data *) ((char *) head + sizeof (struct pthread_log_head));
	}
	// Make the specified number of fake syscalls to sequence signals correctly
	for (i = 0; i < data->retval; i++) {
	    pthread_sysign ();
	}
	// New should be able to consider the next record (after ones handled by signhandler)
	data = head->next;
    }

    if (data->check != check || data->type != type) {
	pthread_log_debug ("Replay mismatch: log record %lu has type %lu check %x - but called with type %lu check %lx\n", data->clock, data->type, data->check, type, check);
	pthread_log_debug ("Callee 0 is 0x%p\n", __builtin_return_address(0));
	pthread_log_debug ("Callee 1 is 0x%p\n", __builtin_return_address(1));
	pthread_log_debug ("Callee 2 is 0x%p\n", __builtin_return_address(2));
	pthread_log_debug ("Callee 3 is 0x%p\n", __builtin_return_address(3));
	exit (0);
    }

    while (*ppthread_log_clock < data->clock) {
	pthread_log_block (data->clock); // Kernel will block us until we can run again
    }
    (*ppthread_log_clock)++;
    DPRINT ("Replay clock incremented to %d\n", *ppthread_log_clock);
    (head->next)++; // Increment to next log record
    if (head->next == head->end) {
	DPRINT ("Log is full - need to reload\n");
	pthread_log_full ();
	head->next = (struct pthread_log_data *) ((char *) head + sizeof (struct pthread_log_head));
    }

    return data->retval;
}

#else
void
pthread_log_record (int retval, unsigned long type, unsigned long check, int is_entry)
{
    struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);
    unsigned long new_clock, delta_clock, log_entry;

    if (head == NULL) return;

    new_clock = atomic_exchange_and_add (ppthread_log_clock, 1);
    delta_clock = new_clock - head->expected_clock; // Store delta-1, not absolute value
    head->expected_clock = new_clock + 1;

    if (retval == 0 && head->need_fake_calls == 0 && delta_clock == 0) {
	head->num_expected_records++;
    } else {
	log_entry = head->num_expected_records; // XXX - assume that this value not greater than CLOCK_MASK
	if (retval) log_entry |= NONZERO_RETVAL_FLAG;
	if (head->need_fake_calls > 0) log_entry |= FAKE_CALLS_FLAG;
	if (delta_clock) log_entry |= SKIPPED_CLOCK_FLAG;
	*((int *) head->next) = log_entry;
	head->next += sizeof(int);

	if (delta_clock) {
	    *((int *) head->next) = delta_clock;
	    head->next += sizeof(unsigned long);
	}
	    
	if (retval) {
	    *((int *) head->next) = retval;
	    head->next += sizeof(int);
	}

	if (head->need_fake_calls > 0) {
	    // Some signals were delivered while we were ignoring syscalls
	    // Add a record via recursive call to generate fake syscalls on replay
	    *((int *) head->next) = head->need_fake_calls;
	    head->need_fake_calls = 0;
	    head->next += sizeof(int);
	}

	if (head->next >= head->end) {
	    pthread_log_full();
	    head->next = ((char *) head + sizeof (struct pthread_log_head));
	}
	head->num_expected_records = 0;
    }

    head->ignore_flag = is_entry;
    DPRINT ("Added record to log: clock %lu retval %d type %lu check %lx\n", new_clock, retval, type, check);
}

int 
pthread_log_replay (unsigned long type, unsigned long check)
{
    struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);
    unsigned long* pentry;
    unsigned long next_clock;
    int num_fake_calls, i, retval;

    if (head == NULL) return 0;

    if (head->num_expected_records > 1) {
	head->expected_clock++;
	(*ppthread_log_clock)++;
	DPRINT ("Replay clock auto-incremented (1) to %d\n", *ppthread_log_clock);
	head->num_expected_records--;
	return 0;
    } 

    pentry = (unsigned long *) head->next;
    
    if (head->num_expected_records == 0) {
	// Only do these things on a *newly read* record
	DPRINT ("Read entry from log: %lx\n", *pentry);

	if (*pentry == 0) { // We've reached the end of the log - some other thread may be able to run, though
	    pthread_log_block (INT_MAX); 
	}
	head->num_expected_records = (*pentry & CLOCK_MASK);
	if (head->num_expected_records) {
	    head->expected_clock++;
	    (*ppthread_log_clock)++;
	    DPRINT ("Replay clock auto-incremented (2) to %d\n", *ppthread_log_clock);
	    return 0;
	}
    }

    head->next += sizeof(unsigned long); // Consume entry since all expected records are done
    head->num_expected_records = 0;

    next_clock = head->expected_clock;
    if (*pentry & SKIPPED_CLOCK_FLAG) {
	next_clock += *((int *)head->next);
	head->next += sizeof(unsigned long);
    }

    if (*pentry & NONZERO_RETVAL_FLAG) {
	retval = *((int *)head->next);
	head->next += sizeof(int);
    } else {
	retval = 0;
    }

    if (*pentry & FAKE_CALLS_FLAG) {
	num_fake_calls = *((int *)head->next);
	head->next += sizeof(int);

	// Make the specified number of fake syscalls to sequence signals correctly
	for (i = 0; i < num_fake_calls; i++) pthread_sysign ();
    }

    while (*ppthread_log_clock < next_clock) {
	DPRINT ("waiting for clock %lu\n", next_clock);
	pthread_log_block (next_clock); // Kernel will block us until we can run again
    }
    head->expected_clock = next_clock + 1;
    (*ppthread_log_clock)++;
    DPRINT ("Replay clock incremented to %d\n", *ppthread_log_clock);
    
    if (head->next >= head->end) {
	DPRINT ("Log is full - need to reload\n");
	pthread_log_full ();
	head->next = ((char *) head + sizeof (struct pthread_log_head));
    }

    return retval;
}
#endif

int
__pthread_mutex_lock (mutex)
     pthread_mutex_t *mutex;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_MUTEX_LOCK_ENTER, (u_long) mutex, 1); 
    rc = __internal_pthread_mutex_lock (mutex);
    pthread_log_record (rc, PTHREAD_MUTEX_LOCK_EXIT, (u_long) mutex, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_MUTEX_LOCK_ENTER, (u_long) mutex); 
    rc = pthread_log_replay (PTHREAD_MUTEX_LOCK_EXIT, (u_long) mutex); 
  } else {
    rc = __internal_pthread_mutex_lock (mutex);
  }
  return rc;
}

strong_alias (__pthread_mutex_lock, pthread_mutex_lock)
strong_alias (__pthread_mutex_lock, __pthread_mutex_lock_internal)

int
pthread_barrier_destroy (barrier)
     pthread_barrier_t *barrier;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_BARRIER_DESTROY_ENTER, (u_long) barrier, 1); 
    rc = internal_pthread_barrier_destroy (barrier);
    pthread_log_record (rc, PTHREAD_BARRIER_DESTROY_EXIT, (u_long) barrier, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_BARRIER_DESTROY_ENTER, (u_long) barrier); 
    rc = pthread_log_replay (PTHREAD_BARRIER_DESTROY_EXIT, (u_long) barrier); 
  } else {
    rc = internal_pthread_barrier_destroy (barrier);
  }
  return rc;
}

int
pthread_barrier_wait (barrier)
     pthread_barrier_t *barrier;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_BARRIER_WAIT_ENTER, (u_long) barrier, 1); 
    rc = internal_pthread_barrier_wait (barrier);
    pthread_log_record (rc, PTHREAD_BARRIER_WAIT_EXIT, (u_long) barrier, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_BARRIER_WAIT_ENTER, (u_long) barrier); 
    rc = pthread_log_replay (PTHREAD_BARRIER_WAIT_EXIT, (u_long) barrier); 
  } else {
    rc = internal_pthread_barrier_wait (barrier);
  }
  return rc;
}

int
__pthread_cond_broadcast (cond)
     pthread_cond_t *cond;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_COND_BROADCAST_ENTER, (u_long) cond, 1); 
    rc = __internal_pthread_cond_broadcast (cond);
    pthread_log_record (rc, PTHREAD_COND_BROADCAST_EXIT, (u_long) cond, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_COND_BROADCAST_ENTER, (u_long) cond); 
    rc = pthread_log_replay (PTHREAD_COND_BROADCAST_EXIT, (u_long) cond); 
  } else {
    rc = __internal_pthread_cond_broadcast (cond);
  }

  return rc;
}

versioned_symbol (libpthread, __pthread_cond_broadcast, pthread_cond_broadcast, GLIBC_2_3_2);

int
__pthread_cond_destroy (cond)
     pthread_cond_t *cond;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_COND_DESTROY_ENTER, (u_long) cond, 1); 
    rc = __internal_pthread_cond_destroy (cond);
    pthread_log_record (rc, PTHREAD_COND_DESTROY_EXIT, (u_long) cond, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_COND_DESTROY_ENTER, (u_long) cond); 
    rc = pthread_log_replay (PTHREAD_COND_DESTROY_EXIT, (u_long) cond); 
  } else {
    rc = __internal_pthread_cond_destroy (cond);
  }

  return rc;
}

versioned_symbol (libpthread, __pthread_cond_destroy, pthread_cond_destroy, GLIBC_2_3_2);

int
__pthread_cond_signal (cond)
     pthread_cond_t *cond;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_COND_SIGNAL_ENTER, (u_long) cond, 1); 
    rc = __internal_pthread_cond_signal (cond);
    pthread_log_record (rc, PTHREAD_COND_SIGNAL_EXIT, (u_long) cond, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_COND_SIGNAL_ENTER, (u_long) cond); 
    rc = pthread_log_replay (PTHREAD_COND_SIGNAL_EXIT, (u_long) cond); 
  } else {
    rc = __internal_pthread_cond_signal (cond);
  }

  return rc;
}

versioned_symbol (libpthread, __pthread_cond_signal, pthread_cond_signal, GLIBC_2_3_2);

int
__pthread_cond_timedwait (cond, mutex, abstime)
     pthread_cond_t *cond;
     pthread_mutex_t *mutex;
     const struct timespec *abstime;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_COND_TIMEDWAIT_ENTER, (u_long) cond, 1); 
    rc = __internal_pthread_cond_timedwait (cond, mutex, abstime);
    pthread_log_record (rc, PTHREAD_COND_TIMEDWAIT_EXIT, (u_long) cond, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_COND_TIMEDWAIT_ENTER, (u_long) cond); 
    rc = pthread_log_replay (PTHREAD_COND_TIMEDWAIT_EXIT, (u_long) cond); 
  } else {
    rc = __internal_pthread_cond_timedwait (cond, mutex, abstime);
  }

  return rc;
}

versioned_symbol (libpthread, __pthread_cond_timedwait, pthread_cond_timedwait, GLIBC_2_3_2);

int
__pthread_cond_wait (cond, mutex)
     pthread_cond_t *cond;
     pthread_mutex_t *mutex;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_COND_WAIT_ENTER, (u_long) cond, 1); 
    rc = __internal_pthread_cond_wait (cond, mutex);
    pthread_log_record (rc, PTHREAD_COND_WAIT_EXIT, (u_long) cond, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_COND_WAIT_ENTER, (u_long) cond); 
    rc = pthread_log_replay (PTHREAD_COND_WAIT_EXIT, (u_long) cond); 
  } else {
    rc = __internal_pthread_cond_wait (cond, mutex);
  }

  return rc;
}

versioned_symbol (libpthread, __pthread_cond_wait, pthread_cond_wait, GLIBC_2_3_2);

int
__pthread_rwlock_rdlock (rwlock)
     pthread_rwlock_t *rwlock;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_RWLOCK_RDLOCK_ENTER, (u_long) rwlock, 1); 
    rc = __internal_pthread_rwlock_rdlock (rwlock);
    pthread_log_record (rc, PTHREAD_RWLOCK_RDLOCK_EXIT, (u_long) rwlock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_RWLOCK_RDLOCK_ENTER, (u_long) rwlock); 
    rc = pthread_log_replay (PTHREAD_RWLOCK_RDLOCK_EXIT, (u_long) rwlock); 
  } else {
    rc = __internal_pthread_rwlock_rdlock (rwlock);
  }

  return rc;
}

weak_alias (__pthread_rwlock_rdlock, pthread_rwlock_rdlock)
strong_alias (__pthread_rwlock_rdlock, __pthread_rwlock_rdlock_internal)

int
__pthread_rwlock_wrlock (rwlock)
     pthread_rwlock_t *rwlock;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_RWLOCK_WRLOCK_ENTER, (u_long) rwlock, 1); 
    rc = __internal_pthread_rwlock_wrlock (rwlock);
    pthread_log_record (rc, PTHREAD_RWLOCK_WRLOCK_EXIT, (u_long) rwlock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_RWLOCK_WRLOCK_ENTER, (u_long) rwlock); 
    rc = pthread_log_replay (PTHREAD_RWLOCK_WRLOCK_EXIT, (u_long) rwlock); 
  } else {
    rc = __internal_pthread_rwlock_wrlock (rwlock);
  }

  return rc;
}

weak_alias (__pthread_rwlock_wrlock, pthread_rwlock_wrlock)
strong_alias (__pthread_rwlock_wrlock, __pthread_rwlock_wrlock_internal)

int
pthread_rwlock_timedrdlock (rwlock, abstime)
     pthread_rwlock_t *rwlock;
     const struct timespec *abstime;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_RWLOCK_TIMEDRDLOCK_ENTER, (u_long) rwlock, 1); 
    rc = internal_pthread_rwlock_timedrdlock (rwlock, abstime);
    pthread_log_record (rc, PTHREAD_RWLOCK_TIMEDRDLOCK_EXIT, (u_long) rwlock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_RWLOCK_TIMEDRDLOCK_ENTER, (u_long) rwlock); 
    rc = pthread_log_replay (PTHREAD_RWLOCK_TIMEDRDLOCK_EXIT, (u_long) rwlock); 
  } else {
    rc = internal_pthread_rwlock_timedrdlock (rwlock, abstime);
  }

  return rc;
}

int
pthread_rwlock_timedwrlock (rwlock, abstime)
     pthread_rwlock_t *rwlock;
     const struct timespec *abstime;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_RWLOCK_TIMEDWRLOCK_ENTER, (u_long) rwlock, 1); 
    rc = internal_pthread_rwlock_timedwrlock (rwlock, abstime);
    pthread_log_record (rc, PTHREAD_RWLOCK_TIMEDWRLOCK_EXIT, (u_long) rwlock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_RWLOCK_TIMEDWRLOCK_ENTER, (u_long) rwlock); 
    rc = pthread_log_replay (PTHREAD_RWLOCK_TIMEDWRLOCK_EXIT, (u_long) rwlock); 
  } else {
    rc = internal_pthread_rwlock_timedwrlock (rwlock, abstime);
  }

  return rc;
}

int
__pthread_rwlock_tryrdlock (rwlock)
     pthread_rwlock_t *rwlock;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_RWLOCK_TRYRDLOCK_ENTER, (u_long) rwlock, 1); 
    rc = __internal_pthread_rwlock_tryrdlock (rwlock);
    pthread_log_record (rc, PTHREAD_RWLOCK_TRYRDLOCK_EXIT, (u_long) rwlock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_RWLOCK_TRYRDLOCK_ENTER, (u_long) rwlock); 
    rc = pthread_log_replay (PTHREAD_RWLOCK_TRYRDLOCK_EXIT, (u_long) rwlock); 
  } else {
    rc = __internal_pthread_rwlock_tryrdlock (rwlock);
  }

  return rc;
}

strong_alias (__pthread_rwlock_tryrdlock, pthread_rwlock_tryrdlock)

int
__pthread_rwlock_trywrlock (rwlock)
     pthread_rwlock_t *rwlock;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_RWLOCK_TRYWRLOCK_ENTER, (u_long) rwlock, 1); 
    rc = __internal_pthread_rwlock_trywrlock (rwlock);
    pthread_log_record (rc, PTHREAD_RWLOCK_TRYWRLOCK_EXIT, (u_long) rwlock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_RWLOCK_TRYWRLOCK_ENTER, (u_long) rwlock); 
    rc = pthread_log_replay (PTHREAD_RWLOCK_TRYWRLOCK_EXIT, (u_long) rwlock); 
  } else {
    rc = __internal_pthread_rwlock_trywrlock (rwlock);
  }

  return rc;
}

strong_alias (__pthread_rwlock_trywrlock, pthread_rwlock_trywrlock)

int
__pthread_rwlock_unlock (pthread_rwlock_t *rwlock)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_RWLOCK_UNLOCK_ENTER, (u_long) rwlock, 1); 
    rc = __internal_pthread_rwlock_unlock (rwlock);
    pthread_log_record (rc, PTHREAD_RWLOCK_UNLOCK_EXIT, (u_long) rwlock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_RWLOCK_UNLOCK_ENTER, (u_long) rwlock); 
    rc = pthread_log_replay (PTHREAD_RWLOCK_UNLOCK_EXIT, (u_long) rwlock); 
  } else {
    rc = __internal_pthread_rwlock_unlock (rwlock);
  }

  return rc;
}

weak_alias (__pthread_rwlock_unlock, pthread_rwlock_unlock)
strong_alias (__pthread_rwlock_unlock, __pthread_rwlock_unlock_internal)

int
pthread_spin_lock (lock)
     pthread_spinlock_t *lock;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_SPIN_LOCK_ENTER, (u_long) lock, 1); 
    rc = internal_pthread_spin_lock (lock);
    pthread_log_record (rc, PTHREAD_SPIN_LOCK_EXIT, (u_long) lock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_SPIN_LOCK_ENTER, (u_long) lock); 
    rc = pthread_log_replay (PTHREAD_SPIN_LOCK_EXIT, (u_long) lock); 
  } else {
    rc = internal_pthread_spin_lock (lock);
  }

  return rc;
}

int
pthread_spin_trylock (lock)
     pthread_spinlock_t *lock;
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_SPIN_TRYLOCK_ENTER, (u_long) lock, 1); 
    rc = internal_pthread_spin_trylock (lock);
    pthread_log_record (rc, PTHREAD_SPIN_TRYLOCK_EXIT, (u_long) lock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_SPIN_TRYLOCK_ENTER, (u_long) lock); 
    rc = pthread_log_replay (PTHREAD_SPIN_TRYLOCK_EXIT, (u_long) lock); 
  } else {
    rc = internal_pthread_spin_trylock (lock);
  }

  return rc;
}

int
pthread_spin_unlock (pthread_spinlock_t *lock)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, PTHREAD_SPIN_UNLOCK_ENTER, (u_long) lock, 1); 
    rc = internal_pthread_spin_unlock (lock);
    pthread_log_record (rc, PTHREAD_SPIN_UNLOCK_EXIT, (u_long) lock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (PTHREAD_SPIN_UNLOCK_ENTER, (u_long) lock); 
    rc = pthread_log_replay (PTHREAD_SPIN_UNLOCK_EXIT, (u_long) lock); 
  } else {
    rc = internal_pthread_spin_unlock (lock);
  }

  return rc;
}

int __new_sem_post (sem_t *__sem)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, SEM_POST_ENTER, (u_long) __sem, 1); 
    rc = __new_internal_sem_post (__sem);
    pthread_log_record (rc, SEM_POST_EXIT, (u_long) __sem, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (SEM_POST_ENTER, (u_long) __sem); 
    rc = pthread_log_replay (SEM_POST_EXIT, (u_long) __sem); 
  } else {
    rc = __new_internal_sem_post (__sem);
  }

  return rc;
  
}
versioned_symbol(libpthread, __new_sem_post, sem_post, GLIBC_2_1);

#if SHLIB_COMPAT(libpthread, GLIBC_2_0, GLIBC_2_1)
int 
attribute_compat_text_section
__old_sem_post (sem_t *__sem)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, SEM_POST_ENTER, (u_long) __sem, 1); 
    rc = __old_internal_sem_post (__sem);
    pthread_log_record (rc, SEM_POST_EXIT, (u_long) __sem, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (SEM_POST_ENTER, (u_long) __sem); 
    rc = pthread_log_replay (SEM_POST_EXIT, (u_long) __sem); 
  } else {
    rc = __old_internal_sem_post (__sem);
  }

  return rc;
  
}
compat_symbol(libpthread, __old_sem_post, sem_post, GLIBC_2_0);
#endif

int __new_sem_wait (sem_t *__sem)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, SEM_WAIT_ENTER, (u_long) __sem, 1); 
    rc = __new_internal_sem_wait (__sem);
    pthread_log_record (rc, SEM_WAIT_EXIT, (u_long) __sem, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (SEM_WAIT_ENTER, (u_long) __sem); 
    rc = pthread_log_replay (SEM_WAIT_EXIT, (u_long) __sem); 
  } else {
    rc = __new_internal_sem_wait (__sem);
  }

  return rc;
  
}
versioned_symbol(libpthread, __new_sem_wait, sem_wait, GLIBC_2_1);

#if SHLIB_COMPAT(libpthread, GLIBC_2_0, GLIBC_2_1)
int 
attribute_compat_text_section
__old_sem_wait (sem_t *__sem)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, SEM_WAIT_ENTER, (u_long) __sem, 1); 
    rc = __old_internal_sem_wait (__sem);
    pthread_log_record (rc, SEM_WAIT_EXIT, (u_long) __sem, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (SEM_WAIT_ENTER, (u_long) __sem); 
    rc = pthread_log_replay (SEM_WAIT_EXIT, (u_long) __sem); 
  } else {
    rc = __old_internal_sem_wait (__sem);
  }

  return rc;
  
}
compat_symbol(libpthread, __old_sem_wait, sem_wait, GLIBC_2_0);
#endif

int __new_sem_trywait (sem_t *__sem)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, SEM_TRYWAIT_ENTER, (u_long) __sem, 1); 
    rc = __new_internal_sem_trywait (__sem);
    pthread_log_record (rc, SEM_TRYWAIT_EXIT, (u_long) __sem, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (SEM_TRYWAIT_ENTER, (u_long) __sem); 
    rc = pthread_log_replay (SEM_TRYWAIT_EXIT, (u_long) __sem); 
  } else {
    rc = __new_internal_sem_trywait (__sem);
  }

  return rc;
  
}
versioned_symbol(libpthread, __new_sem_trywait, sem_trywait, GLIBC_2_1);

#if SHLIB_COMPAT(libpthread, GLIBC_2_0, GLIBC_2_1)
int 
attribute_compat_text_section
__old_sem_trywait (sem_t *__sem)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, SEM_TRYWAIT_ENTER, (u_long) __sem, 1); 
    rc = __old_internal_sem_trywait (__sem);
    pthread_log_record (rc, SEM_TRYWAIT_EXIT, (u_long) __sem, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (SEM_TRYWAIT_ENTER, (u_long) __sem); 
    rc = pthread_log_replay (SEM_TRYWAIT_EXIT, (u_long) __sem); 
  } else {
    rc = __old_internal_sem_trywait (__sem);
  }

  return rc;
  
}
compat_symbol(libpthread, __old_sem_trywait, sem_trywait, GLIBC_2_0);
#endif

int sem_timedwait (sem_t *__sem, const struct timespec *abstime)
{
  int rc;

  if (is_recording()) {
    pthread_log_record (0, SEM_TIMEDWAIT_ENTER, (u_long) __sem, 1); 
    rc = internal_sem_timedwait (__sem, abstime);
    pthread_log_record (rc, SEM_TIMEDWAIT_EXIT, (u_long) __sem, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (SEM_TIMEDWAIT_ENTER, (u_long) __sem); 
    rc = pthread_log_replay (SEM_TIMEDWAIT_EXIT, (u_long) __sem); 
  } else {
    rc = internal_sem_timedwait (__sem, abstime);
  }

  return rc;
}

static int once_lock = LLL_LOCK_INITIALIZER;
static void (*real_init_routine) (void);

static void shim_init_routine (void)
{
  struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);

  // We want to record this routine - this works because of global once_lock
  head->ignore_flag = 0;
  (*real_init_routine)();
  head->ignore_flag = 1;
}

// I suppose that it could be important *which* thread actually calls the init routine.
// The simplest way to enforce this ordering on replay is to not allow two threads to simultaneously
// call a init_routine.  This is pretty heavyweight.
int
__pthread_once (once_control, init_routine)
     pthread_once_t *once_control;
     void (*init_routine) (void);
{
  int rc, retval;

  if (is_recording()) {
    struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1; // don't record this private lock
    lll_lock (once_lock, LLL_PRIVATE);
    pthread_log_record (0, PTHREAD_ONCE_ENTER, (u_long) once_control, 1); 
    real_init_routine = init_routine;
    rc = __no_pthread_once (once_control, shim_init_routine);
    pthread_log_record (rc, PTHREAD_ONCE_EXIT, (u_long) once_control, 0); 
    head->ignore_flag = 1;
    lll_unlock (once_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    struct pthread_log_head* head = THREAD_GETMEM (THREAD_SELF, log_head);

    pthread_log_replay (PTHREAD_ONCE_ENTER, (u_long) once_control); 
    head->ignore_flag = 1; 
    real_init_routine = init_routine;
    retval = __no_pthread_once (once_control, shim_init_routine);
    head->ignore_flag = 0; 
    rc = pthread_log_replay (PTHREAD_ONCE_EXIT, (u_long) once_control); 

    if (retval != rc) pthread_log_debug ("pthread_once returns %d on recording and %d on replay\n", rc, retval);
  } else {
    rc = __no_pthread_once (once_control, init_routine);
  }

  return rc;
}
strong_alias (__pthread_once, pthread_once)

// I am not sure why there are 2 entry points
int
__pthread_once_internal (once_control, init_routine)
     pthread_once_t *once_control;
     void (*init_routine) (void);
{
  return __pthread_once (once_control, init_routine);
}

void pthread_log_lll_lock (int* plock, int type)
{
  if (is_recording()) { 
    pthread_log_record (0, LLL_LOCK_ENTER, (u_long) plock, 1); 
    lll_lock(*plock, type);
    pthread_log_record (0, LLL_LOCK_EXIT, (u_long) plock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (LLL_LOCK_ENTER, (u_long) plock); 
    pthread_log_replay (LLL_LOCK_EXIT, (u_long) plock); 
  } else {
    lll_lock(*plock, type);
  }
}

void pthread_log_lll_unlock (int* plock, int type)
{
  if (is_recording()) { 
    pthread_log_record (0, LLL_UNLOCK_ENTER, (u_long) plock, 1); 
    lll_unlock(*plock, type);
    pthread_log_record (0, LLL_UNLOCK_EXIT, (u_long) plock, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (LLL_UNLOCK_ENTER, (u_long) plock); 
    pthread_log_replay (LLL_UNLOCK_EXIT, (u_long) plock); 
  } else {
    lll_unlock(*plock, type);
  }
}

void pthread_log_lll_wait_tid (int* ptid)
{
  if (is_recording()) { 
    pthread_log_record (0, LLL_WAIT_TID_ENTER, (u_long) ptid, 1); 
    lll_wait_tid(*ptid);
    pthread_log_record (0, LLL_WAIT_TID_EXIT, (u_long) ptid, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (LLL_WAIT_TID_ENTER, (u_long) ptid); 
    pthread_log_replay (LLL_WAIT_TID_EXIT, (u_long) ptid); 
  } else {
    lll_wait_tid(*ptid);
  }
}

int pthread_log_lll_timedwait_tid (int* ptid, const struct timespec* abstime)
{
  int rc;
  if (is_recording()) { 
    pthread_log_record (0, LLL_TIMEDWAIT_TID_ENTER, (u_long) ptid, 1); 
    rc = lll_timedwait_tid(*ptid, abstime);
    pthread_log_record (rc, LLL_TIMEDWAIT_TID_EXIT, (u_long) ptid, 0); 
  } else if (is_replaying()) {
    pthread_log_replay (LLL_TIMEDWAIT_TID_ENTER, (u_long) ptid); 
    rc = pthread_log_replay (LLL_TIMEDWAIT_TID_EXIT, (u_long) ptid); 
  } else {
    rc = lll_timedwait_tid(*ptid, abstime);
  }
  return rc;
}

static int sync_lock = LLL_LOCK_INITIALIZER;

int pthread_log__sync_add_and_fetch(int* val, int x)
{
  struct pthread_log_head* head;
  int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_ADD_AND_FETCH_ENTER, (u_long) val, 1); 
    ret =  __sync_add_and_fetch(val, x);
    pthread_log_record (0, SYNC_ADD_AND_FETCH_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_ADD_AND_FETCH_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_add_and_fetch(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_ADD_AND_FETCH_EXIT, (u_long) val); 
  } else {
    ret =  __sync_add_and_fetch(val, x);
  }
  return ret;
}

int pthread_log__sync_sub_and_fetch(int* val, int x)
{
  struct pthread_log_head* head;
  int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_SUB_AND_FETCH_ENTER, (u_long) val, 1); 
    ret =  __sync_sub_and_fetch(val, x);
    pthread_log_record (0, SYNC_SUB_AND_FETCH_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_SUB_AND_FETCH_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_sub_and_fetch(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_SUB_AND_FETCH_EXIT, (u_long) val); 
  } else {
    ret =  __sync_sub_and_fetch(val, x);
  }
  return ret;
}

int pthread_log__sync_fetch_and_add(int* val, int x)
{
  struct pthread_log_head* head;
  int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_FETCH_AND_ADD_ENTER, (u_long) val, 1); 
    ret =  __sync_fetch_and_add(val, x);
    pthread_log_record (0, SYNC_FETCH_AND_ADD_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_FETCH_AND_ADD_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_fetch_and_add(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_FETCH_AND_ADD_EXIT, (u_long) val); 
  } else {
    ret =  __sync_fetch_and_add(val, x);
  }
  return ret;
}

int pthread_log__sync_fetch_and_sub(int* val, int x)
{
  struct pthread_log_head* head;
  int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_FETCH_AND_SUB_ENTER, (u_long) val, 1); 
    ret =  __sync_fetch_and_sub(val, x);
    pthread_log_record (0, SYNC_FETCH_AND_SUB_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_FETCH_AND_SUB_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_fetch_and_sub(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_FETCH_AND_SUB_EXIT, (u_long) val); 
  } else {
    ret =  __sync_fetch_and_sub(val, x);
  }
  return ret;
}

int pthread_log__sync_lock_test_and_set(int* val, int x)
{
  struct pthread_log_head* head;
  int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_LOCK_TEST_AND_SET_ENTER, (u_long) val, 1); 
    ret =  __sync_lock_test_and_set(val, x);
    pthread_log_record (0, SYNC_LOCK_TEST_AND_SET_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_LOCK_TEST_AND_SET_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_lock_test_and_set(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_LOCK_TEST_AND_SET_EXIT, (u_long) val); 
  } else {
    ret =  __sync_lock_test_and_set(val, x);
  }
  return ret;
}

int pthread_log__sync_bool_compare_and_swap(int* val, int x, int y)
{
  struct pthread_log_head* head;
  int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_BOOL_COMPARE_AND_SWAP_ENTER, (u_long) val, 1); 
    ret =  __sync_bool_compare_and_swap(val, x, y);
    pthread_log_record (0, SYNC_BOOL_COMPARE_AND_SWAP_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_BOOL_COMPARE_AND_SWAP_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_bool_compare_and_swap(val, x, y);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_BOOL_COMPARE_AND_SWAP_EXIT, (u_long) val); 
  } else {
    ret =  __sync_bool_compare_and_swap(val, x, y);
  }
  return ret;
}

int pthread_log__sync_val_compare_and_swap(int* val, int x, int y)
{
  struct pthread_log_head* head;
  int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_VAL_COMPARE_AND_SWAP_ENTER, (u_long) val, 1); 
    ret =  __sync_val_compare_and_swap(val, x, y);
    pthread_log_record (0, SYNC_VAL_COMPARE_AND_SWAP_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_VAL_COMPARE_AND_SWAP_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_val_compare_and_swap(val, x, y);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_VAL_COMPARE_AND_SWAP_EXIT, (u_long) val); 
  } else {
    ret =  __sync_val_compare_and_swap(val, x, y);
  }
  return ret;
}

unsigned int pthread_log__sync_add_and_fetch_uint(unsigned int* val, unsigned int x)
{
  struct pthread_log_head* head;
  unsigned int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_ADD_AND_FETCH_ENTER, (u_long) val, 1); 
    ret =  __sync_add_and_fetch(val, x);
    pthread_log_record (0, SYNC_ADD_AND_FETCH_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_ADD_AND_FETCH_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_add_and_fetch(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_ADD_AND_FETCH_EXIT, (u_long) val); 
  } else {
    ret =  __sync_add_and_fetch(val, x);
  }
  return ret;
}

unsigned int pthread_log__sync_bool_compare_and_swap_uint(unsigned int* val, unsigned int x, unsigned int y)
{
  struct pthread_log_head* head;
  unsigned int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_BOOL_COMPARE_AND_SWAP_ENTER, (u_long) val, 1); 
    ret =  __sync_bool_compare_and_swap(val, x, y);
    pthread_log_record (0, SYNC_BOOL_COMPARE_AND_SWAP_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_BOOL_COMPARE_AND_SWAP_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_bool_compare_and_swap(val, x, y);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_BOOL_COMPARE_AND_SWAP_EXIT, (u_long) val); 
  } else {
    ret =  __sync_bool_compare_and_swap(val, x, y);
  }
  return ret;
}

unsigned int pthread_log__sync_sub_and_fetch_uint(unsigned int* val, unsigned int x)
{
  struct pthread_log_head* head;
  unsigned int ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_SUB_AND_FETCH_ENTER, (u_long) val, 1); 
    ret =  __sync_sub_and_fetch(val, x);
    pthread_log_record (0, SYNC_SUB_AND_FETCH_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_SUB_AND_FETCH_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_sub_and_fetch(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_SUB_AND_FETCH_EXIT, (u_long) val); 
  } else {
    ret =  __sync_sub_and_fetch(val, x);
  }
  return ret;
}

uint64_t pthread_log__sync_add_and_fetch_uint64(uint64_t* val, uint64_t x)
{
  struct pthread_log_head* head;
  uint64_t ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_ADD_AND_FETCH_ENTER, (u_long) val, 1); 
    ret =  __sync_add_and_fetch(val, x);
    pthread_log_record (0, SYNC_ADD_AND_FETCH_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_ADD_AND_FETCH_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_add_and_fetch(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_ADD_AND_FETCH_EXIT, (u_long) val); 
  } else {
    ret =  __sync_add_and_fetch(val, x);
  }
  return ret;
}

uint64_t pthread_log__sync_sub_and_fetch_uint64(uint64_t* val, uint64_t x)
{
  struct pthread_log_head* head;
  uint64_t ret;

  if (is_recording()) { 
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    head->ignore_flag = 1;
    // Enforce that only one thread can be doing a sync operation at a time
    lll_lock(sync_lock, LLL_PRIVATE);
    pthread_log_record (0, SYNC_SUB_AND_FETCH_ENTER, (u_long) val, 1); 
    ret =  __sync_sub_and_fetch(val, x);
    pthread_log_record (0, SYNC_SUB_AND_FETCH_EXIT, (u_long) val, 1); 
    lll_unlock(sync_lock, LLL_PRIVATE);
    head->ignore_flag = 0;
  } else if (is_replaying()) {
    head = THREAD_GETMEM (THREAD_SELF, log_head);

    // Enforced ordering ensures deterministic result
    pthread_log_replay (SYNC_SUB_AND_FETCH_ENTER, (u_long) val); 
    head->ignore_flag = 1;
    ret =  __sync_sub_and_fetch(val, x);
    head->ignore_flag = 0;
    pthread_log_replay (SYNC_SUB_AND_FETCH_EXIT, (u_long) val); 
  } else {
    ret =  __sync_sub_and_fetch(val, x);
  }
  return ret;
}
