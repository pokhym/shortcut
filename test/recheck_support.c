#include <sys/types.h>

#ifndef __USE_LARGEFILE64
#  define __USE_LARGEFILE64
#endif
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <sys/prctl.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
// Note assert requires locale, which does not work with our hacked libc - don't use it */

#include "../dift/recheck_log.h"
#include "taintbuf.h"

static struct go_live_clock* go_live_clock;

#define MAX_THREAD_NUM 99
#define PRINT_VALUES
#define PRINT_TO_LOG
//#define PRINT_SCHEDULING
//#define PRINT_TIMING

#ifdef PRINT_VALUES
static char logbuf[4096];
#endif

// This pauses for a while to let us see what went wrong
#define DELAY
//#define DELAY sleep(2);
#ifdef PRINT_TIMING
unsigned long long success_syscalls[512];
unsigned long long failed_syscalls[512];
unsigned long long success_functions[512];
struct timeval global_start_time_tv;
struct timeval global_end_time_tv;
struct timeval global_start_time_tv_func;
struct timeval global_end_time_tv_func;

inline void start_timing (void) 
{ 
    syscall(SYS_gettimeofday, &global_start_time_tv, NULL);
}

inline void end_timing (int syscall_num, int retval) 
{ 
    unsigned long time;
    syscall (SYS_gettimeofday, &global_end_time_tv, NULL);
    time = global_end_time_tv.tv_usec;
    if (global_end_time_tv.tv_usec < global_start_time_tv.tv_usec) {
        time += 1000000;
    }
    time -= global_start_time_tv.tv_usec;
    if (retval >= 0) { 
        success_syscalls[syscall_num] += time;
    } else { 
        failed_syscalls[syscall_num] += time;
    }
}

inline void start_timing_func (void) 
{ 
    syscall(SYS_gettimeofday, &global_start_time_tv_func, NULL);
}

inline void end_timing_func (int syscall_num) 
{ 
    unsigned long time;
    syscall (SYS_gettimeofday, &global_end_time_tv_func, NULL);
    time = global_end_time_tv_func.tv_usec;
    if (global_end_time_tv_func.tv_usec < global_start_time_tv_func.tv_usec) {
        time += 1000000;
    }
    time -= global_start_time_tv_func.tv_usec;
    success_functions[syscall_num] += time;
}

inline void print_timings (void)
{
    int i = 0;
    struct timeval tv;

    syscall (SYS_gettimeofday, &tv, NULL);
    fprintf (stderr, "successed syscalls %ld.%06ld\n", tv.tv_sec, tv.tv_usec);
    for (i=0; i<512; ++i) { 
        if (success_syscalls[i]) { 
            fprintf (stderr, "%d:%llu\n", i, success_syscalls[i]);
        }
    }
    fprintf (stderr, "failed syscalls\n");
    for (i=0; i<512; ++i) { 
        if (failed_syscalls[i]) { 
            fprintf (stderr, "%d:%llu\n", i, failed_syscalls[i]);
        }
    }
}
#else
#define start_timing(x)
#define end_timing(x,y)
#define start_timing_func(x)
#define end_timing_func(x)
#endif

static char buf[2*1024*1024];
static char tmpbuf[1024*1024];
static char taintbuf_filename[256];
static char slicelog_filename[256];
static char* bufptr = buf;

struct cfopened {
    int is_open_cache_file;
    struct open_retvals orv;
};

#define MAX_FDS 4096
static struct cfopened cache_files_opened[MAX_FDS];

static char taintbuf[1024*1024];
static u_long taintndx = 0;
static u_long last_clock = 0;

static void add_to_taintbuf (struct recheck_entry* pentry, short rettype, void* values, u_long size)
{
    if (taintndx + sizeof(struct taint_retval) + size > sizeof(taintbuf)) {
	fprintf (stderr, "taintbuf full\n");
	abort();
    }
    struct taint_retval* rv = (struct taint_retval *) &taintbuf[taintndx];
    rv->syscall = pentry->sysnum;
    rv->clock = pentry->clock;
    rv->rettype = rettype;
    rv->size = size;
    taintndx += sizeof (struct taint_retval);
    memcpy (&taintbuf[taintndx], values, size);
    taintndx += size;
}

static int dump_taintbuf (u_long diverge_type, u_long diverge_ndx)
{
    long rc;
    int i;

    // We need to dump ALL the taintbufs for every slice here - this will only work 
    // for multithreaded apps - not for multiprocess.
    for (i = 0; i < go_live_clock->num_threads; i++) {
	char dump_filename[256];
	struct taintbuf_hdr hdr;
	int fd;

	if (go_live_clock->process_map[i].taintbuf && *go_live_clock->process_map[i].taintndx) {
	    sprintf (dump_filename, "%s%d", taintbuf_filename, go_live_clock->process_map[i].record_pid);
	    fd = open (dump_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	    if (fd < 0) {
		fprintf (stderr, "Cannot open taint buffer dump file\n");
		return fd;
	    }
	    
	    hdr.diverge_type = diverge_type;
	    hdr.diverge_ndx = diverge_ndx;
	    hdr.last_clock = last_clock;
	    rc = write (fd, &hdr, sizeof(hdr));
	    if (rc != sizeof(hdr)) {
		fprintf (stderr, "Tried to write %d byte header to taint buffer file, rc=%ld\n", sizeof(hdr), rc);
		return -1;
	    }

	    rc = write (fd, go_live_clock->process_map[i].taintbuf, *go_live_clock->process_map[i].taintndx);
	    if (rc != *go_live_clock->process_map[i].taintndx) {
		fprintf (stderr, "Tried to write %ld bytes to taint buffer file, rc=%ld\n", 
			 *go_live_clock->process_map[i].taintndx, rc);
		return -1;
	    }

	    close (fd);
	}
    }

    return 0;
}

void recheck_start(char* filename, void* clock_addr, pid_t record_pid)
{
    int rc, i, fd;
    struct timeval tv;

    start_timing_func ();
    syscall (SYS_gettimeofday, &tv, NULL);
#if 0
    fprintf (stderr, "recheck_start time %ld.%06ld, recheckfile %s, recheckfilename %p(%p), clock_addr %p(%p), %p record pid %d\n", 
	     tv.tv_sec, tv.tv_usec, filename, filename, &filename, clock_addr, &clock_addr, (void*)(*(long*) filename), record_pid);
#endif
    go_live_clock = clock_addr;
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
	fprintf (stderr, "Cannot open recheck file\n");
	return;
    }
    rc = read (fd, buf, sizeof(buf));
    if (rc <= 0) {
	fprintf (stderr, "Cannot read recheck file\n");
	return;
    }
    close (fd);

    for (i = 0; i < MAX_FDS; i++) {
	cache_files_opened[i].is_open_cache_file = 0;
    }

    // Update shared data with taintbuf info
    for (i = 0; i < go_live_clock->num_threads; i++) {
	if (go_live_clock->process_map[i].record_pid == record_pid) {
	    go_live_clock->process_map[i].taintbuf = taintbuf;
	    go_live_clock->process_map[i].taintndx = &taintndx;
	    break;
	} 
    }

    strcpy(taintbuf_filename, filename);
    for (i = strlen(taintbuf_filename)-1; i >= 0; i--) {
	if (taintbuf_filename[i] == '/') {
	    // Will postpend pids for each thread if dumping taint buffer
	    strcpy (&taintbuf_filename[i+1], "taintbuf.");
	    break;
	}
    }

#ifdef PRINT_VALUES
#ifdef PRINT_TO_LOG
    strcpy(slicelog_filename, filename);
    for (i = strlen(slicelog_filename)-1; i >= 0; i--) {
	if (slicelog_filename[i] == '/') {
	    // This will leave the pid appended to filename
	    memcpy (slicelog_filename+i+1, "slicelg", 7); 
	    break;
	}
    }
    fd = open (slicelog_filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);    
    if (fd < 0) {
	fprintf (stderr, "Error opening slice log %s\n", slicelog_filename);
    } else {
	close (fd);
    }
#endif
#endif
#ifdef PRINT_TIMING
    memset (success_syscalls, 0, sizeof(unsigned long long)*512);
    memset (failed_syscalls, 0, sizeof(unsigned long long)*512);
    memset (success_functions, 0, sizeof(unsigned long long)*512);
#endif
    end_timing_func (0);
}

#ifdef PRINT_TO_LOG
#define LPRINT(args...) { int fd;					\
	sprintf (logbuf, args);						\
	fd = open (slicelog_filename, O_WRONLY|O_APPEND, 0644);		\
	if (fd >= 0) {							\
	    if (write (fd, logbuf, strlen(logbuf))			\
		!= strlen(logbuf)) {					\
		fprintf (stderr, "cannot write to log %s\n",		\
			 slicelog_filename);				\
	    }								\
	    close (fd);							\
	} else {							\
	    fprintf (stderr, "cannot log to %s\n", slicelog_filename);	\
	}								\
    }
#else
#define LPRINT printf
#endif

void print_value (u_long foo) 
{
    fprintf (stderr, "print_value: %lu (0x%lx)\n", foo, foo);
}

void handle_mismatch()
{
    //TODO: uncomment this
    dump_taintbuf (DIVERGE_MISMATCH, 0);
    fprintf (stderr, "[MISMATCH] exiting.\n\n\n");
    LPRINT ("[MISMATCH] exiting.\n\n\n");
#ifdef PRINT_VALUES
    fflush (stdout);
#endif
    /*DELAY;
    syscall(350, 2, taintbuf_filename); // Call into kernel to recover transparently
    fprintf (stderr, "handle_jump_diverge: should not get here\n");
    abort();*/
}

void handle_jump_diverge()
{
    int i;
    dump_taintbuf (DIVERGE_JUMP, *((u_long *) ((u_long) &i + 32)));
    fprintf (stderr, "[MISMATCH] tid %ld control flow diverges at %ld.\n\n\n", syscall (SYS_gettid), *((u_long *) ((u_long) &i + 32)));
#ifdef PRINT_VALUES
    fflush (stderr);
#endif
    DELAY;
    syscall(350, 2, taintbuf_filename); // Call into kernel to recover transparently
    fprintf (stderr, "handle_jump_diverge: should not get here\n");
    abort();
}

void handle_delayed_jump_diverge()
{
    int i;
    dump_taintbuf (DIVERGE_JUMP_DELAYED, *((u_long *) ((u_long) &i + 32)));
    fprintf (stderr, "[MISMATCH] control flow delayed divergence");
#ifdef PRINT_VALUES
    fflush (stderr);
#endif
    DELAY;
    syscall(350, 2, taintbuf_filename); // Call into kernel to recover transparently
    fprintf (stderr, "handle_jump_diverge: should not get here\n");
    abort();
}

void handle_index_diverge(u_long foo, u_long bar, u_long baz, u_long quux)
{
    int i;
    dump_taintbuf (DIVERGE_INDEX, *((u_long *) ((u_long) &i + 32)));
    fprintf (stderr, "[MISMATCH] tid %ld index diverges at 0x%lx.\n\n\n", syscall (SYS_gettid), *((u_long *) ((u_long) &i + 32)));
    DELAY;
    syscall(350, 2, taintbuf_filename); // Call into kernel to recover transparently
    fprintf (stderr, "handle_index_diverge: should not get here\n");
    abort ();
}

static inline void check_retval (const char* name, u_long clock, int expected, int actual) {
    if (actual >= 0){
	if (expected != actual) {
	    fprintf (stderr, "[MISMATCH] retval for %s at clock %ld expected %d ret %d\n", name, clock, expected, actual);
            //if divergence happens on open, check what files are currently opened
            if (!strcmp (name, "open")) { 
                int max = expected > actual?expected:actual;
                int i = 0;
                for (i = 3; i<=max; ++i) {
                    char proclnk[256];
                    char filename[256];
                    int r = 0;

                    sprintf(proclnk, "/proc/self/fd/%d", i);
                    r = readlink(proclnk, filename, 255);
                    if (r < 0)
                    {
                        fprintf (stderr, "[BUG] failed to readlink\n\n\n");
                        sleep (2);
                    }
                    filename[r] = '\0';
                    printf ("      file descript %d, filename %s\n", i, filename);
                }
            }
	    handle_mismatch();
	}
    } else {
	if (expected != -1*(errno)) {
	    fprintf (stderr, "[MISMATCH] retval for %s at clock %ld expected %d ret %d\n", name, clock, expected, -1*(errno));
	    handle_mismatch();
	}  
    }
}

void partial_read (struct recheck_entry* pentry, struct read_recheck* pread, char* newdata, char* olddata, int is_cache_file, long total_size) { 
#ifdef PRINT_VALUES
    //only verify bytes not in this range
    int pass = 1;
    LPRINT ("partial read: %d %d %ld\n", pread->partial_read_start, pread->partial_read_end, total_size);
#endif
    if (pread->partial_read_start > 0) { 
        if (memcmp (newdata, olddata, pread->partial_read_start)) {
            printf ("[MISMATCH] read returns different values for partial read: before start\n");
            handle_mismatch();
#ifdef PRINT_VALUES
	    pass = 0;
#endif
        }
    }
    if(pread->partial_read_end > total_size) {
	    printf ("[BUG] partial_read_end out of boundary.\n");
            pread->partial_read_end = total_size;
    }
    if (pread->partial_read_end < total_size) { 
	    if (is_cache_file == 0) {
		    if (memcmp (newdata+pread->partial_read_end, olddata+pread->partial_read_end, total_size-pread->partial_read_end)) {
			    printf ("[MISMATCH] read returns different values for partial read: after end\n");
			    handle_mismatch();
#ifdef PRINT_VALUES
			    pass = 0;
#endif
		    }
	    } else { 
		    //for cached files, we only have the data that needs to be verified
		    if (memcmp (newdata+pread->partial_read_end, olddata+pread->partial_read_start, total_size-pread->partial_read_end)) {
			    printf ("[MISMATCH] read returns different values for partial read: after end\n");
			    handle_mismatch();
#ifdef PRINT_VALUES
			    pass = 0;
#endif
		    }
	    }
    }
    //copy other bytes to the actual address
    memcpy (pread->buf+pread->partial_read_start, newdata+pread->partial_read_start, pread->partial_read_end-pread->partial_read_start);
    add_to_taintbuf (pentry, RETBUF, newdata, total_size);
#ifdef PRINT_VALUES
    if (pass) {
	LPRINT ("partial_read: pass.\n");
    } else {
	LPRINT ("partial_read: verification fails.\n");
    }
#endif
}

long read_recheck (size_t count)
{
    struct recheck_entry* pentry;
    struct read_recheck* pread;
    u_int is_cache_file = 0;
    size_t use_count;
    int rc, i;
    start_timing_func ();

    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pread = (struct read_recheck *) bufptr;
    char* readData = bufptr+sizeof(*pread);
    bufptr += pentry->len;

    if (pread->has_retvals) {
	is_cache_file = *((u_int *)readData);
	readData += sizeof(u_int);
    }
#ifdef PRINT_VALUES
    LPRINT ( "read: has ret vals %d ", pread->has_retvals);
    if (pread->has_retvals) {
	LPRINT ( "is_cache_file: %x ", is_cache_file);
    }
    LPRINT ( "fd %d buf %lx count %d/%d tainted? %d readlen %d returns %ld max %ld clock %lu\n", 
	     pread->fd, (u_long) pread->buf, pread->count, count, pread->is_count_tainted, pread->readlen, pentry->retval, pread->max_bound, pentry->clock);
#endif

    if (pread->is_count_tainted) {
	use_count = count;
    } else {
	use_count = pread->count;
    }

    if ((is_cache_file&IS_PIPE)==IS_PIPE) {
	readData += sizeof(uint64_t) + sizeof(int);
    }
    if ((is_cache_file&CACHE_MASK) && pentry->retval >= 0) {
	struct stat64 st;
	if (!cache_files_opened[pread->fd].is_open_cache_file) {
	    printf ("[BUG] cache file should be opened but it is not, fd should be %d\n", pread->fd);
	    handle_mismatch();
	}
        if (!pread->partial_read) {
            if (fstat64 (pread->fd, &st) < 0) {
                printf ("[MISMATCH] cannot fstat file\n");
                handle_mismatch ();
            }
            if (st.st_mtim.tv_sec == cache_files_opened[pread->fd].orv.mtime.tv_sec &&
                    st.st_mtim.tv_nsec == cache_files_opened[pread->fd].orv.mtime.tv_nsec) {
                if (lseek(pread->fd, pentry->retval, SEEK_CUR) < 0) {
                    printf ("[MISMATCH] lseek after read failed\n");
                    handle_mismatch();
                }
            } else {
                printf ("[BUG] - read file times mismatch but could check actual file content to see if it still matches\n");
		printf ("]BUG] - file system time %ld.%ld cache time %ld.%ld\n", st.st_mtim.tv_sec, st.st_mtim.tv_nsec, cache_files_opened[pread->fd].orv.mtime.tv_sec, cache_files_opened[pread->fd].orv.mtime.tv_nsec);
                handle_mismatch();
            }
        } else {
            //read the new content that will be verified
            start_timing();
            rc = syscall(SYS_read, pread->fd, tmpbuf, use_count);
            end_timing (SYS_read, rc);
	    if (rc != pentry->retval) {
		printf ("[ERROR] retval %d instead of %ld for partial read\n", rc, pentry->retval);
		handle_mismatch();
	    }
	    partial_read (pentry, pread, tmpbuf, (char*)pread+sizeof(*pread)+pread->readlen, 1, rc);
        }
    } else {
	if (pentry->retval > (long) sizeof(tmpbuf)) {
	    printf ("[ERROR] retval %ld is greater than temp buf size %d\n", pentry->retval, sizeof(tmpbuf));
	    handle_mismatch();
	}
	if (use_count > (long) sizeof(tmpbuf)) {
	    printf ("[ERROR] count %d is greater than temp buf size %d\n", use_count, sizeof(tmpbuf));
	    handle_mismatch();
	}
        start_timing();
	rc = syscall(SYS_read, pread->fd, tmpbuf, use_count);
        end_timing (SYS_read, rc);
	if (pread->max_bound > 0) {
	    if (rc > pread->max_bound) {
		printf ("[MISMATCH] read expected up to %d bytes, actually read %ld at clock %ld\n", 
			rc, pread->max_bound, pentry->clock);
		handle_mismatch();
	    } 
	    if (rc > 0) {
		// Read allowed to return different values b/c they are tainted in slice
		// So we copy to the slice address space
		memcpy (pread->buf, tmpbuf, rc);
		add_to_taintbuf (pentry, RETVAL, &rc, sizeof(long));
		add_to_taintbuf (pentry, RETBUF, tmpbuf, rc);
	    }
	} else {
	    check_retval ("read", pentry->clock, pentry->retval, rc);
	    if (!pread->partial_read) {
		if (rc > 0) {
		    if (memcmp (tmpbuf, readData, rc)) {
			printf ("[MISMATCH] read returns different values\n");
			LPRINT ("[MISMATCH] read returns different values - read/expected:\n");
			for (i = 0; i < rc; i++) {
			    if (tmpbuf[i] != readData[i]) LPRINT ("*");
			    LPRINT ("%02x/%02x ", tmpbuf[i]&0xff, readData[i]&0xff);
			    if (i%16 == 15) LPRINT ("\n");
			}
			LPRINT ("\n");
			handle_mismatch();
		    }
		}
	    } else {
		partial_read (pentry, pread, tmpbuf, readData, 0, rc);
	    }
	}
    }
    end_timing_func (SYS_read);
    return rc;
}

#ifdef PRINT_VALUES
inline void print_buffer (u_char* buffer, int len)
{
    int i;
    LPRINT ("{");
    for (i = 0; i < len; i++) { 
	u_char ch = buffer[i];
	if (ch >= 32 && ch <= 126) {
	    LPRINT ("%c", ch);
	} else {
	    LPRINT ("\\%o", ch);
	}
    }
    LPRINT ("}\n");
}
#endif

long recv_recheck ()
{
    struct recheck_entry* pentry;
    struct recv_recheck* precv;
    long rc;
    int i;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    precv = (struct recv_recheck *) bufptr;
    char* recvData = bufptr + sizeof(struct recv_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("recv: sockfd %d buf %p len %d flags %d returns %ld clock %lu buffer offset %ld\n", 
	    precv->sockfd, precv->buf, precv->len, precv->flags, pentry->retval, pentry->clock, (u_long) precv - (u_long) buf);
#endif

    if (pentry->retval == -EAGAIN) {
	LPRINT ("recv: just skip this to emulate timing\n");
	errno = EAGAIN;
	return -1;
    }

    u_long block[6];
    block[0] = precv->sockfd;
    block[1] = (u_long) precv->buf;
    block[2] = precv->len;
    block[3] = precv->flags;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_RECV, &block);
    LPRINT ("recv: returns %ld errno %d\n", rc, errno);
    end_timing (SYS_socketcall, rc);

#ifdef PRINT_VALUES
    print_buffer (precv->buf, rc);
#endif
    // Hack to investigate X behavior - skip events?
    if (rc-pentry->retval == 32) {
	LPRINT ("Trying to skip X event - ugh - just a temporary? hack!!!\n");
	for (i = 0; i < pentry->retval; i++) {
	    ((char *)precv->buf)[i] = ((char *)precv->buf)[i+32];
	}
	rc = pentry->retval;
    }
    check_retval ("recv", pentry->clock, pentry->retval, rc);
    if (rc > 0) {
	LPRINT ("About to compare %p and %p\n", precv->buf, recvData);
	if (precv->partial_read) {
	    if (precv->partial_read_start > 0) {
		if (memcmp (precv->buf, recvData, precv->partial_read_start)) {
		    printf ("[MISMATCH] partial recv start returns different values\n");
		    LPRINT ("[MISMATCH] partial recv %lu start returns different values - read/expected:\n", pentry->clock);
		    for (i = 0; i < precv->partial_read_start; i++) {
			if (((char *)precv->buf)[i] != recvData[i]) LPRINT ("%d ", i);
		    }
		    LPRINT ("\n");
		    
		    handle_mismatch();
		}
	    } 
	    if (precv->partial_read_end < rc) {
		if (memcmp (precv->buf+precv->partial_read_end, recvData+precv->partial_read_end, 
			    rc-precv->partial_read_end)) {
		    printf ("[MISMATCH] partial recv end returns different values\n");
		    LPRINT ("[MISMATCH] partial recv %lu end returns different values - read/expected:\n", pentry->clock);
		    for (i = precv->partial_read_end; i < rc; i++) {
			if (((char *)precv->buf)[i] != recvData[i]) LPRINT ("%d ", i);
		    }
		    handle_mismatch();
		}
	    }
	    add_to_taintbuf (pentry, RETBUF, precv->buf, rc);
	} else {
	    if (memcmp (precv->buf, recvData, rc)) {
		printf ("[MISMATCH] recv returns different values\n");
		LPRINT ("[MISMATCH] recv %lu returns different values - read/expected:\n", pentry->clock);
		if (memcmp (precv->buf, recvData, rc)) {
		    for (i = 0; i < rc; i++) {
			LPRINT ("%02x/%02x ", ((char *)precv->buf)[i], recvData[i]);
			if (i%16 == 15) LPRINT ("\n");
		    }
		    LPRINT ("\n");
		    for (i = 0; i < rc; i++) {
			if (((char *)precv->buf)[i] != recvData[i]) LPRINT ("%d ", i);
		    }
		    LPRINT ("\n");
		    handle_mismatch();
		}
	    }
	}
    }
    end_timing_func (SYS_socketcall);
    return rc;
}

long recvmsg_recheck ()
{
    struct recheck_entry* pentry;
    struct recvmsg_recheck* precvmsg;
    u_long to_cmp;
    int rc, i;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    precvmsg = (struct recvmsg_recheck *) bufptr;
    char* data = bufptr + sizeof (struct recvmsg_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("recvmsg: sockfd %d msg %lx flags %x returns %ld clock %lu\n", 
	     precvmsg->sockfd, (u_long) precvmsg->msg, precvmsg->flags, pentry->retval, pentry->clock);
    if (precvmsg->partial_read) LPRINT ("         partial read start %d end %d\n", precvmsg->partial_read_start, precvmsg->partial_read_end);
#endif

    memcpy (precvmsg->msg, data, sizeof(struct msghdr));
    LPRINT ("recvmsg: namelen %d iovlen %d controllen %d\n", 
	    precvmsg->msg->msg_namelen, precvmsg->msg->msg_iovlen, precvmsg->msg->msg_controllen);
    data += sizeof(struct msghdr);
    memcpy (precvmsg->msg->msg_iov, data, sizeof(struct iovec)*precvmsg->msg->msg_iovlen);
    data += sizeof(struct iovec)*precvmsg->msg->msg_iovlen;

    u_long block[6];
    block[0] = precvmsg->sockfd;
    block[1] = (u_long) precvmsg->msg;
    block[2] = precvmsg->flags;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_RECVMSG, &block);
    end_timing (SYS_socketcall, rc);

    check_retval ("recvmsg", pentry->clock, pentry->retval, rc);
    if (rc >= 0) {
	struct recvmsg_retvals* pretvals = (struct recvmsg_retvals *) data;
	data += sizeof(struct recvmsg_retvals);
	LPRINT ("namelen %d controllen %ld flags %x\n", pretvals->msg_namelen, pretvals->msg_controllen,
		pretvals->msg_flags);
	if (pretvals->msg_namelen != precvmsg->msg->msg_namelen) {
	    fprintf (stderr, "recvmsg returns namelen %d instead of %d\n", precvmsg->msg->msg_namelen, pretvals->msg_namelen);
	    handle_mismatch();
	}
	if (pretvals->msg_controllen != precvmsg->msg->msg_controllen) {
	    fprintf (stderr, "recvmsg returns controllen %d instead of %ld\n", precvmsg->msg->msg_controllen, pretvals->msg_controllen);
	    handle_mismatch();
	}
	if (pretvals->msg_flags != precvmsg->msg->msg_flags) {
	    fprintf (stderr, "recvmsg returns controllen %d instead of %d\n", precvmsg->msg->msg_flags, pretvals->msg_flags);
	    handle_mismatch();
	}
	if (pretvals->msg_namelen > 0) {
	    if (memcmp(data, precvmsg->msg->msg_name, precvmsg->msg->msg_namelen)) {
		fprintf (stderr, "recvmsg returns different name: %s instead of %s\n", data, (char *) precvmsg->msg->msg_name);
		handle_mismatch();
	    }
	}
	if (pretvals->msg_controllen > 0) {
	    if (memcmp(data, precvmsg->msg->msg_control, precvmsg->msg->msg_controllen)) {
		fprintf (stderr, "recvmsg returns different control: %s instead of %s\n", data, (char *) precvmsg->msg->msg_control);
		handle_mismatch();
	    }
	}
	if (precvmsg->partial_read) {
	    u_long compared = 0;
	    int j, mismatch = 0;
	    for (i = 0; i < precvmsg->msg->msg_iovlen; i++) {
		for (j = 0; j < precvmsg->msg->msg_iov[i].iov_len; j++) {
		    if (compared < precvmsg->partial_read_start || 
			compared >= precvmsg->partial_read_end) {
			if (data[compared] != ((char *) precvmsg->msg->msg_iov[i].iov_base)[j]) {
			    LPRINT("byte %lu iovec %u offset %u differs\n", compared, i, j);
			    mismatch = 1;
			}
		    }
		    compared++;
		}
	    }
	    if (mismatch) handle_mismatch();
	} else {
	    int remaining_data = rc;
	    for (i = 0; i < precvmsg->msg->msg_iovlen; i++) {
		to_cmp = precvmsg->msg->msg_iov[i].iov_len;
		if (to_cmp < rc) to_cmp = rc;
		if (memcmp (precvmsg->msg->msg_iov[i].iov_base, data, to_cmp)) {
		    u_int j;
		    fprintf (stderr, "recvmsg differs in data in iov %d\n", i);
		    print_buffer (precvmsg->msg->msg_iov[i].iov_base, to_cmp);
		    print_buffer ((u_char *) data, to_cmp);
		    for (j = 0; j < to_cmp; j++) {
			if (((char *) precvmsg->msg->msg_iov[i].iov_base)[j] != data[j]) {
			    LPRINT ("%d ", j);
			}
		    }
		    LPRINT ("differs\n");
		    handle_mismatch();
		}
		data += to_cmp;
		remaining_data -= to_cmp;
	    }
	}
    }
    end_timing_func (SYS_socketcall);
    return rc;
}

static inline void fill_taintedbuf(char* data, char* buf, u_long len)
{
    u_long i;
    char* tainted = data;
    char* outbuf = data + len;

    for (i = 0; i < len; i++) {
	if (!tainted[i] && buf[i] != outbuf[i]) buf[i] = outbuf[i];
    }
}

long write_recheck ()
{
    struct recheck_entry* pentry;
    struct write_recheck* pwrite;
    char* data;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pwrite = (struct write_recheck *) bufptr;
    data = bufptr + sizeof(struct write_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "write: fd %d buf %lx count %d rc %ld clock %lu\n", pwrite->fd, (u_long) pwrite->buf, pwrite->count, pentry->retval, pentry->clock);
#endif
    if (pwrite->fd == 99999) return pwrite->count;  // Debugging fd - ignore
    if (cache_files_opened[pwrite->fd].is_open_cache_file) {
	printf ("[ERROR] Should not be writing to a cache file\n");
	handle_mismatch();
    }
    fill_taintedbuf (data, (char *) pwrite->buf, pwrite->count);

    start_timing();
    rc = syscall(SYS_write, pwrite->fd, pwrite->buf, pwrite->count);
    end_timing(SYS_write, rc);
    check_retval ("write", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_write);
    return rc;
}

long writev_recheck ()
{
    struct recheck_entry* pentry;
    struct writev_recheck* pwritev;
    char* data;
    int rc, i;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pwritev = (struct writev_recheck *) bufptr;
    data = bufptr + sizeof(struct writev_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("writev: fd %d iov %p iovcnt %d rc %ld clock %lu\n", pwritev->fd, pwritev->iov, pwritev->iovcnt, pentry->retval, pentry->clock);
#endif
    if (cache_files_opened[pwritev->fd].is_open_cache_file) {
	printf ("[ERROR] Should not be writing to a cache file\n");
	handle_mismatch();
    }
    memcpy (pwritev->iov, data, pwritev->iovcnt * sizeof(struct iovec));
    data += pwritev->iovcnt * sizeof(struct iovec);
    for (i = 0; i < pwritev->iovcnt; i++) {
	fill_taintedbuf (data, pwritev->iov[i].iov_base, pwritev->iov[i].iov_len);
	data += pwritev->iov[i].iov_len*2;
    }

    start_timing();
    rc = syscall(SYS_writev, pwritev->fd, pwritev->iov, pwritev->iovcnt);
    end_timing(SYS_writev, rc);
    check_retval ("writev", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_writev);
    return rc;
}

long send_recheck ()
{
    struct recheck_entry* pentry;
    struct send_recheck* psend;
    char* data;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psend = (struct send_recheck *) bufptr;
    data = bufptr + sizeof(struct send_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "send: sockfd %d buf %p len %d flags %d rc %ld clock %lu\n", psend->sockfd, psend->buf, psend->len, psend->flags, pentry->retval, pentry->clock);
#endif

    fill_taintedbuf (data, psend->buf, psend->len);

    u_long block[6];
    block[0] = psend->sockfd;
    block[1] = (u_long) psend->buf;
    block[2] = psend->len;
    block[3] = psend->flags;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_SEND, &block);
    end_timing(SYS_socketcall, rc);
    check_retval ("send", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_socketcall);
    return rc;
}

long sendmsg_recheck ()
{
    struct recheck_entry* pentry;
    struct sendmsg_recheck* psendmsg;
    char* data;
    u_int i;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psendmsg = (struct sendmsg_recheck *) bufptr;
    data = bufptr + sizeof(struct sendmsg_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "sendmsg: sockfd %d msg %p flags %d rc %ld clock %lu\n", psendmsg->sockfd, psendmsg->msg, psendmsg->flags, pentry->retval, pentry->clock);
#endif

    memcpy (psendmsg->msg, data, sizeof(struct msghdr));
    data += sizeof(struct msghdr);
    memcpy (psendmsg->msg->msg_name, data, psendmsg->msg->msg_namelen);
    data += psendmsg->msg->msg_namelen;
    memcpy (psendmsg->msg->msg_iov, data, psendmsg->msg->msg_iovlen*sizeof(struct iovec));
    data += psendmsg->msg->msg_iovlen*sizeof(struct iovec);
    for (i = 0; i < psendmsg->msg->msg_iovlen; i++) {
	fill_taintedbuf (data, psendmsg->msg->msg_iov[i].iov_base, psendmsg->msg->msg_iov[i].iov_len);
	data += psendmsg->msg->msg_iov[i].iov_len*2;
    }
    fill_taintedbuf (data, psendmsg->msg->msg_control, psendmsg->msg->msg_controllen);

    u_long block[6];
    block[0] = psendmsg->sockfd;
    block[1] = (u_long) psendmsg->msg;
    block[2] = psendmsg->flags;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_SENDMSG, &block);
    LPRINT ("sendmsg rc %d errno %d\n", rc, errno);
    end_timing(SYS_socketcall, rc);
    check_retval ("sendmsg", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_socketcall);
    return rc;
}

long open_recheck (int mode)
{
    struct recheck_entry* pentry;
    struct open_recheck* popen;
    int use_mode, rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    popen = (struct open_recheck *) bufptr;
    char* fileName = bufptr+sizeof(struct open_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "open: filename %s flags %x mode %d tainted? %d", fileName, popen->flags, popen->mode, popen->is_mode_tainted);
    if (popen->has_retvals) {
	LPRINT ( " dev %ld ino %ld mtime %ld.%ld", popen->retvals.dev, popen->retvals.ino, 
	       popen->retvals.mtime.tv_sec, popen->retvals.mtime.tv_nsec); 
    }
    LPRINT ( " rc %ld clock %lu, tid %ld, bufptr %p, buf %p\n", pentry->retval, pentry->clock, syscall (SYS_gettid), bufptr, buf);
#endif

    if (popen->is_mode_tainted) {
	use_mode = mode;
    } else {
	use_mode = popen->mode;
    }

    start_timing();
    rc = syscall(SYS_open, fileName, popen->flags, use_mode);
    end_timing (SYS_open, rc);
    check_retval ("open", pentry->clock, pentry->retval, rc);
    if (rc >= MAX_FDS) abort ();
    if (rc >= 0 && popen->has_retvals) {
	cache_files_opened[rc].is_open_cache_file = 1;
	cache_files_opened[rc].orv = popen->retvals;
    }
    end_timing_func (SYS_open);
    return rc;
}

long openat_recheck ()
{
    struct recheck_entry* pentry;
    struct openat_recheck* popen;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    popen = (struct openat_recheck *) bufptr;
    char* fileName = bufptr+sizeof(struct openat_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "openat: dirfd %d filename %s flags %x mode %d rc %ld clock %lu\n", popen->dirfd, fileName, popen->flags, popen->mode, pentry->retval, pentry->clock);
#endif
    start_timing();
    rc = syscall(SYS_openat, popen->dirfd, fileName, popen->flags, popen->mode);
    end_timing (SYS_openat, rc);
    check_retval ("openat", pentry->clock, pentry->retval, rc);
    if  (rc >= MAX_FDS) abort ();
    end_timing_func (SYS_openat);
    return rc;
}

long waitpid_recheck ()
{
    struct recheck_entry* pentry;
    struct waitpid_recheck* pwaitpid;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pwaitpid = (struct waitpid_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES 
    LPRINT ("waitpid: pid_t %d status %p val %d options %d clock %lu\n", pwaitpid->pid, pwaitpid->status, pwaitpid->statusval, pwaitpid->options, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_waitpid, pwaitpid->pid, pwaitpid->status, pwaitpid->options);
    end_timing (SYS_waitpid, rc);
    check_retval ("waitpid", pentry->clock, pentry->retval, rc);
    if (rc <= 0) {
	if (*pwaitpid->status != pwaitpid->statusval) {
	    fprintf (stderr, "waitpid: expected status %d, got %d\n", pwaitpid->statusval, *pwaitpid->status);
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_waitpid);
    return rc;
}

long close_recheck ()
{
    struct recheck_entry* pentry;
    struct close_recheck* pclose;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pclose = (struct close_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES 
    LPRINT ("close: fd %d clock %lu\n", pclose->fd, pentry->clock);
#endif

    if (pclose->fd >= MAX_FDS) abort();
    start_timing();
    rc = syscall(SYS_close, pclose->fd);
    end_timing (SYS_close, rc);
    cache_files_opened[pclose->fd].is_open_cache_file = 0;
    check_retval ("close", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_close);
    return rc;
}

long dup2_recheck ()
{
    struct recheck_entry* pentry;
    struct dup2_recheck* pdup2;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pdup2 = (struct dup2_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES 
    LPRINT ("dup2: oldfd %d newfd %d clock %lu\n", pdup2->oldfd, pdup2->newfd, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_dup2, pdup2->oldfd, pdup2->newfd);
    end_timing (SYS_dup2, rc);
    cache_files_opened[pdup2->newfd].is_open_cache_file = cache_files_opened[pdup2->oldfd].is_open_cache_file;
    check_retval ("dup2", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_dup2);
    return rc;
}

long access_recheck ()
{
    struct recheck_entry* pentry;
    struct access_recheck* paccess;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    paccess = (struct access_recheck *) bufptr;
    char* accessName = bufptr+sizeof(*paccess);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("acccess: mode %d pathname %s rc %ld clock %lu\n", paccess->mode, accessName, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_access, accessName, paccess->mode);
    end_timing(SYS_access, rc);
    check_retval ("access", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_access);
    return rc;
}

long stat64_alike_recheck (char* syscall_name, int syscall_num)
{
    struct recheck_entry* pentry;
    struct stat64_recheck* pstat64;
    struct stat64 st;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pstat64 = (struct stat64_recheck *) bufptr;
    char* pathName = bufptr+sizeof(struct stat64_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "%s: rc %ld pathname %s buf %lx ", syscall_name, pentry->retval, pathName, (u_long) pstat64->buf);
    if (pstat64->has_retvals) {
	LPRINT ( "%s retvals: st_dev %llu st_ino %llu st_mode %d st_nlink %d st_uid %d st_gid %d st_rdev %llu "
	       "st_size %lld st_atime %ld st_mtime %ld st_ctime %ld st_blksize %ld st_blocks %lld clock %lu\n",
	       syscall_name, pstat64->retvals.st_dev, pstat64->retvals.st_ino, pstat64->retvals.st_mode, pstat64->retvals .st_nlink, pstat64->retvals.st_uid,pstat64->retvals .st_gid,
	       pstat64->retvals.st_rdev, pstat64->retvals.st_size, pstat64->retvals .st_atime, pstat64->retvals.st_mtime, pstat64->retvals.st_ctime, pstat64->retvals.st_blksize,
		 pstat64->retvals.st_blocks, pentry->clock); 
    } else {
	LPRINT ( "no return values clock %ld\n", pentry->clock);
    }
#endif

    start_timing();
    rc = syscall(syscall_num, pathName, &st);
    end_timing (syscall_num, rc);
    check_retval (syscall_name, pentry->clock, pentry->retval, rc);
    if (pstat64->has_retvals) {
	if (st.st_dev != pstat64->retvals.st_dev) {
	    printf ("[MISMATCH] %s dev does not match %llu vs. recorded %llu\n", syscall_name, st.st_dev, pstat64->retvals.st_dev);
	    handle_mismatch();
	}
#if 0
	if (st.st_ino != pstat64->retvals.st_ino) {
	    printf ("[MISMATCH] stat64 ino does not match %llu vs. recorded %llu\n", st.st_ino, pstat64->retvals.st_ino);
	    handle_mismatch();
	}
#endif
	if (st.st_mode != pstat64->retvals.st_mode) {
	    printf ("[MISMATCH] %s mode does not match %d vs. recorded %d\n", syscall_name, st.st_mode, pstat64->retvals.st_mode);
	    handle_mismatch();
	}
#if 0
	if (st.st_nlink != pstat64->retvals.st_nlink) {
	    printf ("[MISMATCH] %s nlink does not match %d vs. recorded %d\n",syscall_name,  st.st_nlink, pstat64->retvals.st_nlink);
	    handle_mismatch();
	}
#endif
	if (st.st_uid != pstat64->retvals.st_uid) {
	    printf ("[MISMATCH] %s uid does not match %d vs. recorded %d\n", syscall_name, st.st_uid, pstat64->retvals.st_uid);
	    handle_mismatch();
	}
	if (st.st_gid != pstat64->retvals.st_gid) {
	    printf ("[MISMATCH] %s gid does not match %d vs. recorded %d\n", syscall_name, st.st_gid, pstat64->retvals.st_gid);
	    handle_mismatch();
	}
#if 0
	if (st.st_rdev != pstat64->retvals.st_rdev) {
	    printf ("[MISMATCH] %s rdev does not match %llu vs. recorded %llu\n", syscall_name, st.st_rdev, pstat64->retvals.st_rdev);
	    handle_mismatch();
	}
#endif
	if (st.st_size != pstat64->retvals.st_size) {
	    printf ("[MISMATCH] %s size does not match %lld vs. recorded %lld\n", syscall_name, st.st_size, pstat64->retvals.st_size);
	    handle_mismatch();
	}
#if 0
	if (st.st_mtime != pstat64->retvals.st_mtime) {
	    printf ("[MISMATCH] stat64 mtime does not match %ld vs. recorded %ld\n", st.st_mtime, pstat64->retvals.st_mtime);
	    handle_mismatch();
	}
	if (st.st_ctime != pstat64->retvals.st_ctime) {
	    printf ("[MISMATCH] stat64 ctime does not match %ld vs. recorded %ld\n", st.st_ctime, pstat64->retvals.st_ctime);
	    handle_mismatch();
	}
#endif
	/* Assume atime will be handled by tainting since it changes often */
	((struct stat64 *) pstat64->buf)->st_ino = st.st_ino;
	((struct stat64 *) pstat64->buf)->st_nlink = st.st_nlink;
	((struct stat64 *) pstat64->buf)->st_rdev = st.st_rdev;
	//((struct stat64 *) pstat64->buf)->st_size = st.st_size;
	((struct stat64 *) pstat64->buf)->st_mtime = st.st_mtime;
	((struct stat64 *) pstat64->buf)->st_ctime = st.st_ctime;
	((struct stat64 *) pstat64->buf)->st_atime = st.st_atime;
	//((struct stat64 *) pstat64->buf)->st_blocks = st.st_blocks;
	add_to_taintbuf (pentry, STAT64_INO, &st.st_ino, sizeof(st.st_ino));
	add_to_taintbuf (pentry, STAT64_NLINK, &st.st_nlink, sizeof(st.st_nlink));
	add_to_taintbuf (pentry, STAT64_RDEV, &st.st_rdev, sizeof(st.st_rdev));
	add_to_taintbuf (pentry, STAT64_MTIME, &st.st_mtime, sizeof(st.st_mtime));
	add_to_taintbuf (pentry, STAT64_CTIME, &st.st_ctime, sizeof(st.st_ctime));
	add_to_taintbuf (pentry, STAT64_ATIME, &st.st_atime, sizeof(st.st_atime));
	if (st.st_blksize != pstat64->retvals.st_blksize) {
	    printf ("[MISMATCH] %s blksize does not match %ld vs. recorded %ld\n", syscall_name, st.st_blksize, pstat64->retvals.st_blksize);
	    handle_mismatch();
	}
	if (st.st_blocks != pstat64->retvals.st_blocks) {
	    printf ("[MISMATCH] %s blocks does not match %lld vs. recorded %lld\n", syscall_name, st.st_blocks, pstat64->retvals.st_blocks);
	    handle_mismatch();
	}
    }
    end_timing_func (syscall_num);
    return rc;
}

long stat64_recheck () { 
    return stat64_alike_recheck ("stat64", SYS_stat64);
}

long lstat64_recheck () { 
    return stat64_alike_recheck ("lstat64", SYS_lstat64);
}

long fstat64_recheck ()
{
    struct recheck_entry* pentry;
    struct fstat64_recheck* pfstat64;
    struct stat64 st;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pfstat64 = (struct fstat64_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "fstat64: rc %ld fd %d buf %lx ", pentry->retval, pfstat64->fd, (u_long) pfstat64->buf);
    if (pfstat64->has_retvals) {
	LPRINT ( "st_dev %llu st_ino %llu st_mode %d st_nlink %d st_uid %d st_gid %d st_rdev %llu "
	       "st_size %lld st_atime %ld st_mtime %ld st_ctime %ld st_blksize %ld st_blocks %lld clock %lu\n",
	       pfstat64->retvals.st_dev, pfstat64->retvals.st_ino, pfstat64->retvals.st_mode, pfstat64->retvals .st_nlink, pfstat64->retvals.st_uid,pfstat64->retvals .st_gid,
	       pfstat64->retvals.st_rdev, pfstat64->retvals.st_size, pfstat64->retvals .st_atime, pfstat64->retvals.st_mtime, pfstat64->retvals.st_ctime, pfstat64->retvals.st_blksize,
		 pfstat64->retvals.st_blocks, pentry->clock); 
    } else {
	LPRINT ( "no return values clock %lu\n", pentry->clock);
    }
#endif

    start_timing();
    rc = syscall(SYS_fstat64, pfstat64->fd, &st);
    end_timing (SYS_fstat64, rc);
    check_retval ("fstat64", pentry->clock, pentry->retval, rc);
    if (pfstat64->has_retvals) {
	if (st.st_dev != pfstat64->retvals.st_dev) {
	    printf ("[MISMATCH] fstat64 dev does not match %llu vs. recorded %llu\n", st.st_dev, pfstat64->retvals.st_dev);
	    handle_mismatch();
	}
#if 0
	if (st.st_ino != pfstat64->retvals.st_ino) {
	    printf ("[MISMATCH] fstat64 ino does not match %llu vs. recorded %llu\n", st.st_ino, pfstat64->retvals.st_ino);
	    handle_mismatch();
	}
#endif
	if (st.st_mode != pfstat64->retvals.st_mode) {
	    printf ("[MISMATCH] fstat64 mode does not match %d vs. recorded %d\n", st.st_mode, pfstat64->retvals.st_mode);
	    handle_mismatch();
	}
#if 0
	if (st.st_nlink != pfstat64->retvals.st_nlink) {
	    printf ("[MISMATCH] fstat64 nlink does not match %d vs. recorded %d\n", st.st_nlink, pfstat64->retvals.st_nlink);
	    handle_mismatch();
	}
#endif
	if (st.st_uid != pfstat64->retvals.st_uid) {
	    printf ("[MISMATCH] fstat64 uid does not match %d vs. recorded %d\n", st.st_uid, pfstat64->retvals.st_uid);
	    handle_mismatch();
	}
	if (st.st_gid != pfstat64->retvals.st_gid) {
	    printf ("[MISMATCH] fstat64 gid does not match %d vs. recorded %d\n", st.st_gid, pfstat64->retvals.st_gid);
	    handle_mismatch();
	}
#if 0
	if (st.st_rdev != pfstat64->retvals.st_rdev) {
	    printf ("[MISMATCH] fstat64 rdev does not match %llu vs. recorded %llu\n", st.st_rdev, pfstat64->retvals.st_rdev);
	    handle_mismatch();
	}
#endif
	if (st.st_size != pfstat64->retvals.st_size) {
	    printf ("[MISMATCH] fstat64 size does not match %lld vs. recorded %lld\n", st.st_size, pfstat64->retvals.st_size);
	    handle_mismatch();
	}
#if 0
	if (st.st_mtime != pfstat64->retvals.st_mtime) {
	    printf ("[MISMATCH] fstat64 mtime does not match %ld vs. recorded %ld\n", st.st_mtime, pfstat64->retvals.st_mtime);
	    handle_mismatch();
	}
	if (st.st_ctime != pfstat64->retvals.st_ctime) {
	    printf ("[MISMATCH] fstat64 ctime does not match %ld vs. recorded %ld\n", st.st_ctime, pfstat64->retvals.st_ctime);
	    handle_mismatch();
	}
#endif
	/* Assume inode, atime, mtime, ctime will be handled by tainting since it changes often */
	((struct stat64 *) pfstat64->buf)->st_ino = st.st_ino;
	((struct stat64 *) pfstat64->buf)->st_nlink = st.st_nlink;
	((struct stat64 *) pfstat64->buf)->st_rdev = st.st_rdev;
	//((struct stat64 *) pfstat64->buf)->st_size = st.st_size;
	((struct stat64 *) pfstat64->buf)->st_mtime = st.st_mtime;
	((struct stat64 *) pfstat64->buf)->st_ctime = st.st_ctime;
	((struct stat64 *) pfstat64->buf)->st_atime = st.st_atime;
	//((struct stat64 *) pfstat64->buf)->st_blocks = st.st_blocks;
	add_to_taintbuf (pentry, STAT64_INO, &st.st_ino, sizeof(st.st_ino));
	add_to_taintbuf (pentry, STAT64_NLINK, &st.st_nlink, sizeof(st.st_nlink));
	add_to_taintbuf (pentry, STAT64_RDEV, &st.st_rdev, sizeof(st.st_rdev));
	add_to_taintbuf (pentry, STAT64_MTIME, &st.st_mtime, sizeof(st.st_mtime));
	add_to_taintbuf (pentry, STAT64_CTIME, &st.st_ctime, sizeof(st.st_ctime));
	add_to_taintbuf (pentry, STAT64_ATIME, &st.st_atime, sizeof(st.st_atime));
	if (st.st_blksize != pfstat64->retvals.st_blksize) {
	    printf ("[MISMATCH] fstat64 blksize does not match %ld vs. recorded %ld\n", st.st_blksize, pfstat64->retvals.st_blksize);
	    handle_mismatch();
	}
	if (st.st_blocks != pfstat64->retvals.st_blocks) {
	    printf ("[MISMATCH] fstat64 blocks does not match %lld vs. recorded %lld\n", st.st_blocks, pfstat64->retvals.st_blocks);
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_fstat64);
    return rc;
}

long fcntl64_getfd_recheck ()
{
    struct recheck_entry* pentry;
    struct fcntl64_getfd_recheck* pgetfd;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetfd = (struct fcntl64_getfd_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "fcntl64 getfd: fd %d rc %ld clock %lu\n", pgetfd->fd, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_fcntl64, pgetfd->fd, F_GETFD);
    end_timing (SYS_fcntl64, rc);
    check_retval ("fcntl64 getfd", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_fcntl64);
    return rc;
}

long fcntl64_setfd_recheck ()
{
    struct recheck_entry* pentry;
    struct fcntl64_setfd_recheck* psetfd;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psetfd = (struct fcntl64_setfd_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "fcntl64 setfd: fd %d arg %d rc %ld clock %lu\n", psetfd->fd, psetfd->arg, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_fcntl64, psetfd->fd, F_SETFD, psetfd->arg);
    end_timing (SYS_fcntl64, rc);
    check_retval ("fcntl64 setfd", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_fcntl64);
    return rc;
}

long fcntl64_getfl_recheck ()
{
    struct recheck_entry* pentry;
    struct fcntl64_getfl_recheck* pgetfl;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetfl = (struct fcntl64_getfl_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "fcntl64 getfl: fd %d rc %ld clock %lu\n", pgetfl->fd, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_fcntl64, pgetfl->fd, F_GETFL);
    end_timing (SYS_fcntl64, rc);
    check_retval ("fcntl64 getfl", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_fcntl64);
    return rc;
}

long fcntl64_setfl_recheck ()
{
    struct recheck_entry* pentry;
    struct fcntl64_setfl_recheck* psetfl;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psetfl = (struct fcntl64_setfl_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "fcntl64 setfl: fd %d flags %lx rc %ld clock %lu\n", psetfl->fd, psetfl->flags, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_fcntl64, psetfl->fd, F_SETFL, psetfl->flags);
    end_timing (SYS_fcntl64, rc);
    check_retval ("fcntl64 setfl", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_fcntl64);
    return rc;
}

long fcntl64_getlk_recheck ()
{
    struct recheck_entry* pentry;
    struct fcntl64_getlk_recheck* pgetlk;
    struct flock fl;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetlk = (struct fcntl64_getlk_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "fcntl64 getlk: fd %d arg %lx rc %ld clock %lu\n", pgetlk->fd, (u_long) pgetlk->arg, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_fcntl64, pgetlk->fd, F_GETLK, &fl);
    end_timing (SYS_fcntl64, rc);
    check_retval ("fcntl64 getlk", pentry->clock, pentry->retval, rc);
    if (pgetlk->has_retvals) {
	if (memcmp(&fl, &pgetlk->flock, sizeof(fl))) {
	    printf ("[MISMATCH] fcntl64 getlk does not match\n");
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_fcntl64);
    return rc;
}

long fcntl64_getown_recheck ()
{
    struct recheck_entry* pentry;
    struct fcntl64_getown_recheck* pgetown;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetown = (struct fcntl64_getown_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("fcntl64 getown: fd %d rc %ld clock %lu\n", pgetown->fd, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_fcntl64, pgetown->fd, F_GETOWN);
    end_timing(SYS_fcntl64, rc);
    check_retval ("fcntl64 getown", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_fcntl64);
    return rc;
}

long fcntl64_setown_recheck (long owner)
{
    struct recheck_entry* pentry;
    struct fcntl64_setown_recheck* psetown;
    long use_owner;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psetown = (struct fcntl64_setown_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "fcntl64 setown: fd %d owner %lx rc %ld clock %lu\n", psetown->fd, psetown->owner, pentry->retval, pentry->clock);
#endif

    if (psetown->is_owner_tainted) {
	use_owner = owner; 
    } else {
	use_owner = psetown->owner;
    }

    start_timing();
    rc = syscall(SYS_fcntl64, psetown->fd, F_SETOWN, use_owner);
    end_timing (SYS_fcntl64, rc);
    check_retval ("fcntl64 setown", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_fcntl64);
    return rc;
}

long ugetrlimit_recheck ()
{
    struct recheck_entry* pentry;
    struct ugetrlimit_recheck* pugetrlimit;
    struct rlimit rlim;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pugetrlimit = (struct ugetrlimit_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "ugetrlimit: resource %d rlimit %ld %ld rc %ld clock %lu\n", pugetrlimit->resource, pugetrlimit->rlim.rlim_cur, pugetrlimit->rlim.rlim_max, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_ugetrlimit, pugetrlimit->resource, &rlim);
    end_timing (SYS_ugetrlimit, rc);
    check_retval ("ugetrlimit", pentry->clock, pentry->retval, rc);
    if (memcmp(&rlim, &pugetrlimit->rlim, sizeof(rlim))) {
	printf ("[MISMATCH] ugetrlimit does not match: returns %ld %ld, while in recheck log %ld %ld, on resource %d\n", rlim.rlim_cur, rlim.rlim_max, pugetrlimit->rlim.rlim_cur, pugetrlimit->rlim.rlim_max, pugetrlimit->resource);
	handle_mismatch();
    }
    end_timing_func (SYS_ugetrlimit);
    return rc;
}

long setrlimit_recheck ()
{
    struct recheck_entry* pentry;
    struct setrlimit_recheck* psetrlimit;
    long rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psetrlimit = (struct setrlimit_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "setrlimit: resource %d rlimit %ld %ld rc %ld clock %lu\n", psetrlimit->resource, psetrlimit->rlim.rlim_cur, psetrlimit->rlim.rlim_max, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_setrlimit, psetrlimit->resource, &psetrlimit->rlim);
    end_timing (SYS_setrlimit, rc);
    check_retval ("setrlimit", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_setrlimit);
    return rc;
}

long uname_recheck ()
{
    struct recheck_entry* pentry;
    struct uname_recheck* puname;
    struct utsname uname;
    int rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    puname = (struct uname_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "uname: sysname %s nodename %s release %s version %s machine %s rc %ld clock %lu\n", 
	     puname->utsname.sysname, puname->utsname.nodename, puname->utsname.release, puname->utsname.version, puname->utsname.machine, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_uname, &uname);
    end_timing (SYS_uname, rc);
    check_retval ("uname", pentry->clock, pentry->retval, rc);

    if (memcmp(&uname.sysname, &puname->utsname.sysname, sizeof(uname.sysname))) {
	fprintf (stderr, "[MISMATCH] uname sysname does not match: %s\n", uname.sysname);
	handle_mismatch();
    }
    if (memcmp(&uname.nodename, &puname->utsname.nodename, sizeof(uname.nodename))) {
	fprintf (stderr, "[MISMATCH] uname nodename does not match: %s\n", uname.nodename);
	handle_mismatch();
    }
    if (memcmp(&uname.release, &puname->utsname.release, sizeof(uname.release))) {
	fprintf (stderr, "[MISMATCH] uname release does not match: %s\n", uname.release);
	handle_mismatch();
    }
    /* Assume version will be handled by tainting since it changes often */
#ifdef PRINT_VALUES
    LPRINT ( "Buffer is %lx\n", (u_long) puname->buf);
    LPRINT ( "Copy to version buffer at %lx\n", (u_long) &((struct utsname *) puname->buf)->version);
#endif
    memcpy (&((struct utsname *) puname->buf)->version, &puname->utsname.version, sizeof(puname->utsname.version));
    add_to_taintbuf (pentry, UNAME_VERSION, &puname->utsname.version, sizeof(puname->utsname.version));
    if (memcmp(&uname.machine, &puname->utsname.machine, sizeof(uname.machine))) {
	fprintf (stderr, "[MISMATCH] uname machine does not match: %s\n", uname.machine);
	handle_mismatch();
    }
    end_timing_func (SYS_uname);
    return rc;
}

long statfs64_recheck ()
{
    struct recheck_entry* pentry;
    struct statfs64_recheck* pstatfs64;
    struct statfs64 st;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pstatfs64 = (struct statfs64_recheck *) bufptr;
    char* path = bufptr+sizeof(struct statfs64_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "statfs64: path %s size %u type %d bsize %d blocks %lld bfree %lld bavail %lld files %lld ffree %lld fsid %d %d namelen %d frsize %d rc %ld clock %lu\n", path, pstatfs64->sz,
	   pstatfs64->statfs.f_type, pstatfs64->statfs.f_bsize, pstatfs64->statfs.f_blocks, pstatfs64->statfs.f_bfree, pstatfs64->statfs.f_bavail, pstatfs64->statfs.f_files, 
	     pstatfs64->statfs.f_ffree, pstatfs64->statfs.f_fsid.__val[0], pstatfs64->statfs.f_fsid.__val[1], pstatfs64->statfs.f_namelen, pstatfs64->statfs.f_frsize, pentry->retval, pentry->clock);
#endif

    start_timing();
    rc = syscall(SYS_statfs64, path, pstatfs64->sz, &st);
    end_timing (SYS_statfs64, rc);
    check_retval ("statfs64", pentry->clock, pentry->retval, rc);
    if (rc == 0) {
	if (pstatfs64->statfs.f_type != st.f_type) {
	    fprintf (stderr, "[MISMATCH] statfs64 f_type does not match: %d\n", st.f_type);
	    handle_mismatch();
	}
	if (pstatfs64->statfs.f_bsize != st.f_bsize) {
	    fprintf (stderr, "[MISMATCH] statfs64 f_bsize does not match: %d\n", st.f_bsize);
	    handle_mismatch();
	}
	if (pstatfs64->statfs.f_blocks != st.f_blocks) {
	    fprintf (stderr, "[MISMATCH] statfs64 f_blocks does not match: %lld\n", st.f_blocks);
	    handle_mismatch();
	}
	/* Assume free and available blocks handled by tainting */
	pstatfs64->buf->f_bfree = st.f_bfree;
	add_to_taintbuf (pentry, STATFS64_BFREE, &pstatfs64->buf->f_bfree, sizeof (pstatfs64->buf->f_bfree));
	pstatfs64->buf->f_bavail = st.f_bavail;
	add_to_taintbuf (pentry, STATFS64_BAVAIL, &pstatfs64->buf->f_bavail, sizeof (pstatfs64->buf->f_bavail));
	if (pstatfs64->statfs.f_files != st.f_files) {
	    fprintf (stderr, "[MISMATCH] statfs64 f_bavail does not match: %lld\n", st.f_files);
	    handle_mismatch();
	}
	/* Assume free files handled by tainting */
	pstatfs64->buf->f_ffree = st.f_ffree;
	add_to_taintbuf (pentry, STATFS64_FFREE, &pstatfs64->buf->f_ffree, sizeof (pstatfs64->buf->f_ffree));
	if (pstatfs64->statfs.f_fsid.__val[0] != st.f_fsid.__val[0] || pstatfs64->statfs.f_fsid.__val[1] != st.f_fsid.__val[1]) {
	    fprintf (stderr, "[MISMATCH] statfs64 f_fdid does not match: %d %d\n", st.f_fsid.__val[0],  st.f_fsid.__val[1]);
	    handle_mismatch();
	}
	if (pstatfs64->statfs.f_namelen != st.f_namelen) {
	    fprintf (stderr, "[MISMATCH] statfs64 f_namelen does not match: %d\n", st.f_namelen);
	    handle_mismatch();
	}
	if (pstatfs64->statfs.f_frsize != st.f_frsize) {
	    fprintf (stderr, "[MISMATCH] statfs64 f_frsize does not match: %d\n", st.f_frsize);
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_statfs64);
    return rc;
}

long gettimeofday_recheck () { 
    struct recheck_entry* pentry;
    struct gettimeofday_recheck *pget;
    struct timeval tv;
    struct timezone tz;
    int rc;
    
    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pget = (struct gettimeofday_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "gettimeofday: pointer tv %lx tz %lx clock %lu bufptr %p, buf %p\n", (long) pget->tv_ptr, (long) pget->tz_ptr, pentry->clock, bufptr, buf);
#endif
    start_timing();
    rc = syscall (SYS_gettimeofday, &tv, &tz);
    end_timing (SYS_gettimeofday, rc);
    check_retval ("gettimeofday", pentry->clock, pentry->retval, rc);
    
    if (pget->tv_ptr) { 
	memcpy (pget->tv_ptr, &tv, sizeof(struct timeval));
	add_to_taintbuf (pentry, GETTIMEOFDAY_TV, &tv, sizeof(struct timeval));
    }
    if (pget->tz_ptr) { 
	memcpy (pget->tz_ptr, &tz, sizeof(struct timezone));
	add_to_taintbuf (pentry, GETTIMEOFDAY_TZ, &tz, sizeof(struct timezone));
    }
    end_timing_func (SYS_gettimeofday);
    return rc;
}

long clock_gettime_recheck () 
{
    struct recheck_entry* pentry;
    struct clock_getx_recheck *pget;
    struct timespec tp;
    long rc;
    
    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pget = (struct clock_getx_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("clock_gettime: clockid %d, tp %p clock %lu\n", pget->clk_id, pget->tp, pentry->clock);
#endif
    start_timing();
    rc = syscall (SYS_clock_gettime, pget->clk_id, &tp);
    end_timing (SYS_clock_gettime, rc);
    check_retval ("clock_gettime", pentry->clock, pentry->retval, rc);
    
    if (pget->tp) {
        memcpy (pget->tp, &tp, sizeof(tp));
        add_to_taintbuf (pentry, CLOCK_GETTIME, &tp, sizeof(tp));
    }
    end_timing_func (SYS_clock_gettime);
    return rc;
}

long clock_getres_recheck (int clock_id) 
{
    struct recheck_entry* pentry;
    struct clock_getx_recheck *pget;
    clockid_t clk_id;
    struct timespec tp;
    long rc;
    
    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pget = (struct clock_getx_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("clock_getres: clockid %d, id tainted? %d, new clock id %d, tp %p clock %lu\n", pget->clk_id, pget->clock_id_tainted, clock_id, pget->tp, pentry->clock);
#endif
    if (pget->clock_id_tainted) { 
        clk_id = clock_id;
    } else { 
        clk_id = pget->clk_id;
    }
    start_timing();
    rc = syscall (SYS_clock_getres, clk_id, &tp);
    end_timing (SYS_clock_getres, rc);
    check_retval ("clock_getres", pentry->clock, pentry->retval, rc);
    
    if (pget->tp) {
        memcpy (pget->tp, &tp, sizeof(tp));
        add_to_taintbuf (pentry, CLOCK_GETRES, &tp, sizeof(tp));
    }
    end_timing_func (SYS_clock_getres);
    return rc;
}

long time_recheck () { 
    struct recheck_entry* pentry;
    struct time_recheck *pget;
    int rc;
    
    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pget = (struct time_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("time: pointer t %x clock %lu\n", (int)(pget->t), pentry->clock);
#endif
    start_timing();
    rc = syscall (SYS_time, pget->t);
    end_timing (SYS_time, rc);
    add_to_taintbuf (pentry, RETVAL, &rc, sizeof(long));
    if (rc >= 0 && pget->t) add_to_taintbuf (pentry, RETBUF, pget->t, sizeof(time_t));
    end_timing_func (SYS_time);
    return rc;
}

long prlimit64_recheck ()
{
    struct recheck_entry* pentry;
    struct prlimit64_recheck* prlimit;
    struct rlimit64 rlim;
    struct rlimit64* prlim;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    prlimit = (struct prlimit64_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "prlimit64: pid %d resource %d new limit %lx old limit %lx rc %ld clock %lu\n", prlimit->pid, prlimit->resource, 
	     (u_long) prlimit->new_limit, (u_long) prlimit->old_limit, pentry->retval, pentry->clock);
    if (prlimit->has_retvals) {
	LPRINT ( "old soft limit: %lld hard limit %lld\n", prlimit->retparams.rlim_cur, prlimit->retparams.rlim_max);
    }
#endif
    if (prlimit->old_limit) {
	prlim = &rlim;
	rlim.rlim_cur = rlim.rlim_max = 0;
    } else {
	prlim = NULL;
    }
    start_timing();
    rc = syscall(SYS_prlimit64, prlimit->pid, prlimit->resource, prlimit->new_limit, prlim);
    end_timing (SYS_prlimit64, rc);
    check_retval ("prlimit64", pentry->clock, pentry->retval, rc);
    if (prlimit->has_retvals) {
	if (prlimit->retparams.rlim_cur != rlim.rlim_cur) {
	    printf ("[MISMATCH] prlimit64 soft limit does not match: %lld\n", rlim.rlim_cur);
	}
	if (prlimit->retparams.rlim_max != rlim.rlim_max) {
	    printf ("[MISMATCH] prlimit64 hard limit does not match: %lld\n", rlim.rlim_max);
	}
    }
    end_timing_func (SYS_prlimit64);
    return rc;
}

long setpgid_recheck (int pid, int pgid)
{
    struct recheck_entry* pentry;
    struct setpgid_recheck* psetpgid;
    pid_t use_pid, use_pgid;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psetpgid = (struct setpgid_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "setpgid: pid tainted? %d record pid %d passed pid %d pgid tainted? %d record pgid %d passed pgid %d clock %lu\n", 
	     psetpgid->is_pid_tainted, psetpgid->pid, pid, psetpgid->is_pgid_tainted, psetpgid->pgid, pgid, pentry->clock);
#endif 
    if (psetpgid->is_pid_tainted) {
	use_pid = pid; 
    } else {
	use_pid = psetpgid->pid;
    }
    if (psetpgid->is_pgid_tainted) {
	use_pgid = pgid; 
    } else {
	use_pgid = psetpgid->pgid;
    }

    start_timing();
    rc = syscall(SYS_setpgid, use_pid, use_pgid);
    end_timing(SYS_setpgid, rc);
    check_retval ("setpgid", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_setpgid);
    return rc;
}

long readlink_recheck ()
{
    struct recheck_entry* pentry;
    struct readlink_recheck* preadlink;
    char* linkdata;
    char* path;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    preadlink = (struct readlink_recheck *) bufptr;
    if (pentry->retval > 0) {
	linkdata = bufptr+sizeof(struct readlink_recheck);
	path = linkdata + pentry->retval;
    } else {
	linkdata = NULL;
	path = bufptr+sizeof(struct readlink_recheck);
    }
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "readlink: buf %p size %d ", preadlink->buf, preadlink->bufsiz);
    if (pentry->retval) {
	int i;
	LPRINT ( "linkdata ");
	for (i = 0; i < pentry->retval; i++) {
	    LPRINT ( "%c", linkdata[i]);
	}
	LPRINT ( " ");
    }
    LPRINT ( "path %s rc %ld clock %lu\n", path, pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_readlink, path, tmpbuf, preadlink->bufsiz);
    end_timing (SYS_readlink, rc);
    check_retval ("readlink", pentry->clock, pentry->retval, rc);
    if (rc > 0) {
	if (memcmp(tmpbuf, linkdata, pentry->retval)) {
	    printf ("[MISMATCH] readdata returns link data %s\n", linkdata);
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_readlink);
    return rc;
}

long socket_recheck ()
{
    struct recheck_entry* pentry;
    struct socket_recheck* psocket;
    u_long block[6];
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psocket = (struct socket_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "socket: domain %d type %x protocol %d rc %ld clock %lu\n", psocket->domain, psocket->type, psocket->protocol, pentry->retval, pentry->clock);
#endif 

    block[0] = psocket->domain;
    block[1] = psocket->type;
    block[2] = psocket->protocol;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_SOCKET, &block);
    end_timing (SYS_socketcall, rc);
    check_retval ("socket", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_socketcall);
    return rc;
}

long setsockopt_recheck ()
{
    struct recheck_entry* pentry;
    struct setsockopt_recheck* psetsockopt;
    u_long block[6];
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psetsockopt = (struct setsockopt_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "setsockopt: sockfd %d level %d optname %d optval %p optlen %d rc %ld clock %lu\n", 
	     psetsockopt->sockfd, psetsockopt->level, psetsockopt->optname, psetsockopt->optval, psetsockopt->optlen, pentry->retval, pentry->clock);
#endif 

    block[0] = psetsockopt->sockfd;
    block[1] = psetsockopt->level;
    block[2] = psetsockopt->optname;
    block[3] = (u_long) psetsockopt->optval;
    block[4] = psetsockopt->optlen;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_SETSOCKOPT, &block);
    end_timing (SYS_socketcall, rc);
    check_retval ("setsockopt", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_socketcall);
    return rc;
}

inline void process_taintmask (char* mask, u_long size, char* buffer)
{
    u_long i;
    char* outbuf = mask + size;
    for (i = 0; i < size; i++) {
	if (!mask[i]) buffer[i] = outbuf[i];
    }
}

static inline long connect_or_bind_recheck (int call, char* call_name)
{
    struct recheck_entry* pentry;
    struct connect_recheck* pconnect;
    u_long block[6];
    char* addr;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pconnect = (struct connect_recheck *) bufptr;
    addr = bufptr+sizeof(struct connect_recheck);
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "%s: sockfd %d addlen %d rc %ld clock %lu\n", call_name, pconnect->sockfd, pconnect->addrlen, pentry->retval, pentry->clock);
#endif 
    process_taintmask(addr, pconnect->addrlen, (char *) pconnect->addr);
    {
      int i;
      for (i = 0; i < pconnect->addrlen; i++) {
	u_char ch = ((char *) pconnect->addr)[i];
	if (ch >= 32 && ch <= 126) {
	  LPRINT ("%c", ch);
	} else {
	  LPRINT ("\%u", ch);
	}
      }
    }
    block[0] = pconnect->sockfd;
    block[1] = (u_long) pconnect->addr;
    block[2] = pconnect->addrlen;
    start_timing();
    rc = syscall(SYS_socketcall, call, &block);
    end_timing (SYS_socketcall, rc);
    check_retval (call_name, pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_socketcall);
    return rc;
}

long connect_recheck () { 
    return connect_or_bind_recheck (SYS_CONNECT, "connect");
}

long bind_recheck () {
    return connect_or_bind_recheck (SYS_BIND, "bind");
}

long getsockname_recheck (int call)
{
    struct recheck_entry* pentry;
    struct getsockname_recheck* pgetsockname;
    u_long block[6];
    char* addr;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetsockname = (struct getsockname_recheck *) bufptr;
    addr = bufptr+sizeof(struct getsockname_recheck);
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "getsockname: sockfd %d addr %p addlen %p rc %ld clock %lu\n", 
	     pgetsockname->sockfd, pgetsockname->addr, pgetsockname->addrlen, pentry->retval, pentry->clock);
#endif 
    block[0] = pgetsockname->sockfd;
    block[1] = (u_long) pgetsockname->addr;
    *pgetsockname->addrlen = pgetsockname->addrlenval;
    block[2] = (u_long) pgetsockname->addrlen;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_GETSOCKNAME, &block);
    end_timing (SYS_socketcall, rc);
    check_retval ("getsockname", pentry->clock, pentry->retval, rc);
    if (rc > 0) {
	if (*pgetsockname->addrlen != pgetsockname->arglen) {
	    LPRINT ("getsockname: address length return mismatch: %d vs %ld\n", *pgetsockname->addrlen, pgetsockname->arglen);
	    handle_mismatch();
	}
	if (!memcmp(addr, pgetsockname->addrlen, pgetsockname->arglen)) {
	    LPRINT ("getsockname: address is different %s vs %s\n", addr, (char *) pgetsockname->addr);
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_socketcall);
    return rc;
}

long getpeername_recheck (int call)
{
    struct recheck_entry* pentry;
    struct getpeername_recheck* pgetpeername;
    u_long block[6];
    char* addr;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetpeername = (struct getpeername_recheck *) bufptr;
    addr = bufptr+sizeof(struct getpeername_recheck);
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "getpeername: sockfd %d addr %p addlen %p rc %ld clock %lu\n", 
	     pgetpeername->sockfd, pgetpeername->addr, pgetpeername->addrlen, pentry->retval, pentry->clock);
#endif 
    block[0] = pgetpeername->sockfd;
    block[1] = (u_long) pgetpeername->addr;
    *pgetpeername->addrlen = pgetpeername->addrlenval;
    block[2] = (u_long) pgetpeername->addrlen;
    start_timing();
    rc = syscall(SYS_socketcall, SYS_GETPEERNAME, &block);
    end_timing (SYS_socketcall, rc);
    check_retval ("getpeername", pentry->clock, pentry->retval, rc);
    if (rc > 0) {
	if (*pgetpeername->addrlen != pgetpeername->arglen) {
	    LPRINT ("getpeername: address length return mismatch: %d vs %ld\n", *pgetpeername->addrlen, pgetpeername->arglen);
	    handle_mismatch();
	}
	if (!memcmp(addr, pgetpeername->addrlen, pgetpeername->arglen)) {
	    LPRINT ("getpeername: address is different %s vs %s\n", addr, (char *) pgetpeername->addr);
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_socketcall);
    return rc;
}

long getpid_recheck ()
{
    long rc;
    struct recheck_entry* pentry = (struct recheck_entry *) bufptr;
    start_timing_func ();
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;

#ifdef PRINT_VALUES
    LPRINT ( "getpid: rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getpid);
    end_timing (SYS_getpid, rc);
    add_to_taintbuf (pentry, RETVAL, &rc, sizeof(rc));
    end_timing_func (SYS_getpid);
    return rc;
}

long gettid_recheck ()
{
    long rc;
    struct recheck_entry* pentry = (struct recheck_entry *) bufptr;
    start_timing_func ();
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;

#ifdef PRINT_VALUES
    LPRINT ( "gettid: rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_gettid);
    end_timing (SYS_gettid, rc);
    add_to_taintbuf (pentry, RETVAL, &rc, sizeof(rc));
    end_timing_func (SYS_gettid);
    return rc;
}

long getpgrp_recheck ()
{
    long rc;
    struct recheck_entry* pentry = (struct recheck_entry *) bufptr;
    start_timing_func ();
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;

#ifdef PRINT_VALUES
    LPRINT ("getpgrp: rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc =  syscall(SYS_getpgrp);
    end_timing(SYS_getpgrp, rc);
    add_to_taintbuf (pentry, RETVAL, &rc, sizeof(rc));
    end_timing_func (SYS_getpgrp);
    return rc;
}

long getuid32_recheck ()
{
    struct recheck_entry* pentry;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;

#ifdef PRINT_VALUES
    LPRINT ( "getuid32: rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getuid32);
    end_timing (SYS_getuid32, rc);
    check_retval ("getuid32", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_getuid32);
    return rc;
}

long geteuid32_recheck ()
{
    struct recheck_entry* pentry;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;

#ifdef PRINT_VALUES
    LPRINT ( "geteuid32: rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_geteuid32);
    end_timing (SYS_geteuid32, rc);
    check_retval ("geteuid32", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_geteuid32);
    return rc;
}

long getgid32_recheck ()
{
    struct recheck_entry* pentry;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;

#ifdef PRINT_VALUES
    LPRINT ( "getgid32: rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getgid32);
    end_timing(SYS_getgid32, rc);
    check_retval ("getgid32", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_getgid32);
    return rc;
}

long getegid32_recheck ()
{
    struct recheck_entry* pentry;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;

#ifdef PRINT_VALUES
    LPRINT ( "getegid32: rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getegid32);
    check_retval ("getegid32", pentry->clock, pentry->retval, rc);
    end_timing(SYS_getegid32, rc);
    end_timing_func (SYS_getegid32);
    return rc;
}

long getresuid_recheck ()
{
    struct recheck_entry* pentry;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    struct getresuid_recheck* pgetresuid = (struct getresuid_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "getresuid: ruid %p=%d euid %p=%d guid %p=%d rc %ld clock %lu\n", 
	     pgetresuid->ruid, pgetresuid->ruidval, pgetresuid->euid, pgetresuid->euidval, pgetresuid->suid, pgetresuid->suidval, pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getresuid32, pgetresuid->ruid, pgetresuid->euid, pgetresuid->suid);
    check_retval ("getresuid", pentry->clock, pentry->retval, rc);
    if (rc >= 0) {
	if (*pgetresuid->ruid != pgetresuid->ruidval) {
	    fprintf (stderr, "getresuid: expected ruid %d, got %d\n", pgetresuid->ruidval, *pgetresuid->ruid);
	    handle_mismatch();
	}
	if (*pgetresuid->euid != pgetresuid->euidval) {
	    fprintf (stderr, "getresuid: expected euid %d, got %d\n", pgetresuid->euidval, *pgetresuid->euid);
	    handle_mismatch();
	}
	if (*pgetresuid->suid != pgetresuid->suidval) {
	    fprintf (stderr, "getresuid: expected suid %d, got %d\n", pgetresuid->suidval, *pgetresuid->suid);
	    handle_mismatch();
	}
    }
    end_timing(SYS_getresuid32, rc);
    end_timing_func (SYS_getresuid32);
    return rc;
}

long getresgid_recheck ()
{
    struct recheck_entry* pentry;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    struct getresgid_recheck* pgetresgid = (struct getresgid_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "getresgid: rgid %p=%d egid %p=%d ggid %p=%d rc %ld clock %lu\n", 
	     pgetresgid->rgid, pgetresgid->rgidval, pgetresgid->egid, pgetresgid->egidval, pgetresgid->sgid, pgetresgid->sgidval, pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getresgid32, pgetresgid->rgid, pgetresgid->egid, pgetresgid->sgid);
    check_retval ("getresgid", pentry->clock, pentry->retval, rc);
    if (rc >= 0) {
	if (*pgetresgid->rgid != pgetresgid->rgidval) {
	    fprintf (stderr, "getresgid: expected rgid %d, got %d\n", pgetresgid->rgidval, *pgetresgid->rgid);
	    handle_mismatch();
	}
	if (*pgetresgid->egid != pgetresgid->egidval) {
	    fprintf (stderr, "getresgid: expected egid %d, got %d\n", pgetresgid->egidval, *pgetresgid->egid);
	    handle_mismatch();
	}
	if (*pgetresgid->sgid != pgetresgid->sgidval) {
	    fprintf (stderr, "getresgid: expected sgid %d, got %d\n", pgetresgid->sgidval, *pgetresgid->sgid);
	    handle_mismatch();
	}
    }
    end_timing(SYS_getresgid32, rc);
    end_timing_func (SYS_getresgid32);
    return rc;
}

long llseek_recheck ()
{
    struct recheck_entry* pentry;
    struct llseek_recheck* pllseek;
    loff_t off;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock; 
    pllseek = (struct llseek_recheck *) bufptr;
    bufptr += pentry->len;
   
#ifdef PRINT_VALUES
    LPRINT ( "llseek: fd %u high offset %lx low offset %lx whence %u rc %ld clock %lu", pllseek->fd, pllseek->offset_high, pllseek->offset_low, pllseek->whence, pentry->retval, pentry->clock);
    if (pentry->retval >= 0) {
	LPRINT ( "off %llu\n", pllseek->result);
    } else {
	LPRINT ( "\n");
    }
#endif 

    start_timing();
    rc = syscall(SYS__llseek, pllseek->fd, pllseek->offset_high, pllseek->offset_low, &off, pllseek->whence);
    end_timing (SYS__llseek, rc);
    check_retval ("llseek", pentry->clock, pentry->retval, rc);
    if (rc >= 0 && off != pllseek->result) {
	printf ("[MISMATCH] llseek returns offset %llu\n", off);
	handle_mismatch();
    }
    end_timing_func (SYS__llseek);
    return rc;
}

long ioctl_recheck ()
{
    struct recheck_entry* pentry;
    struct ioctl_recheck* pioctl;
    char* addr;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pioctl = (struct ioctl_recheck *) bufptr;
    addr = bufptr+sizeof(struct ioctl_recheck);
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "ioctl: fd %u cmd %x dir %x size %x arg %lx arglen %ld rc %ld clock %lu\n", pioctl->fd, pioctl->cmd, pioctl->dir, pioctl->size, (u_long) pioctl->arg, pioctl->arglen, pentry->retval, pentry->clock);
#endif 

    if (pioctl->dir == _IOC_WRITE) {
        start_timing();
	rc = syscall(SYS_ioctl, pioctl->fd, pioctl->cmd, tmpbuf);
        end_timing(SYS_ioctl, rc);
	check_retval ("ioctl", pentry->clock, pentry->retval, rc);
	// Right now we are tainting buffer
	memcpy (pioctl->arg, tmpbuf, pioctl->arglen);
	add_to_taintbuf (pentry, RETBUF, tmpbuf, pioctl->arglen);
#ifdef PRINT_VALUES
	if (pioctl->cmd == 0x5413) {
	  short* ps = (short *) &tmpbuf;
	  LPRINT ("window size is %d %d\n", ps[0], ps[1]);
	}
#endif
    } else if (pioctl->dir == _IOC_READ) {
	if (pioctl->size) {
	    fill_taintedbuf (addr, pioctl->arg, pioctl->size);
	}
        start_timing();
	rc = syscall(SYS_ioctl, pioctl->fd, pioctl->cmd, pioctl->arg);
        end_timing (SYS_ioctl, rc);
	check_retval ("ioctl", pentry->clock, pentry->retval, rc);
    } else {
	printf ("[ERROR] ioctl_recheck only handles ioctl dir _IOC_WRITE and _IOC_READ for now\n");
    }
    end_timing_func (SYS_ioctl);
    return rc;
}

// Can I find this definition as user level?
struct linux_dirent {
    unsigned long        d_ino;
    unsigned long        d_off;
    unsigned short	 d_reclen;
    char		 d_name[1];
};

long getdents_recheck ()
{
    struct recheck_entry* pentry;
    struct getdents64_recheck* pgetdents;
    char* dents;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetdents = (struct getdents64_recheck *) bufptr;
    if (pgetdents->arglen > 0) {
	dents = bufptr+sizeof(struct getdents64_recheck);
    }
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "getdents: fd %u buf %p count %u arglen %ld rc %ld clock %lu\n", pgetdents->fd, pgetdents->buf, pgetdents->count, pgetdents->arglen, pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getdents, pgetdents->fd, tmpbuf, pgetdents->count);
    end_timing (SYS_getdents, rc);
    check_retval ("getdents", pentry->clock, pentry->retval, rc);
    if (rc > 0) {
	int compared = 0;
	char* p = dents; 
	char* c = tmpbuf;
	while (compared < rc) {
	    struct linux_dirent* prev = (struct linux_dirent *) p;
	    struct linux_dirent* curr = (struct linux_dirent *) c;
	    if (prev->d_ino != curr->d_ino || prev->d_off != curr->d_off ||
		prev->d_reclen != curr->d_reclen || strcmp(prev->d_name, curr->d_name)) {
		printf ("{MISMATCH] getdetnts: inode %lu vs. %lu\t", prev->d_ino, curr->d_ino);
		printf ("offset %ld vs. %ld\t", prev->d_off, curr->d_off);
		printf ("reclen %d vs. %d\t", prev->d_reclen, curr->d_reclen);
		printf ("name %s vs. %s\t", prev->d_name, curr->d_name);
		handle_mismatch();
	    }
	    if (prev->d_reclen <= 0) break;
	    p += prev->d_reclen; c += curr->d_reclen; compared += prev->d_reclen;
	}
    }
    end_timing_func (SYS_getdents);
    return rc;
}

// Can I find this definition at user level?
struct linux_dirent64 {
	__u64		d_ino;
	__s64		d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char		d_name[0];
};

long getdents64_recheck ()
{
    struct recheck_entry* pentry;
    struct getdents64_recheck* pgetdents64;
    char* dents;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pgetdents64 = (struct getdents64_recheck *) bufptr;
    if (pgetdents64->arglen > 0) {
	dents = bufptr+sizeof(struct getdents64_recheck);
    }
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "getdents64: fd %u buf %p count %u arglen %ld rc %ld clock %lu\n", pgetdents64->fd, pgetdents64->buf, pgetdents64->count, pgetdents64->arglen, pentry->retval, pentry->clock);
#endif 
    start_timing();
    rc = syscall(SYS_getdents64, pgetdents64->fd, tmpbuf, pgetdents64->count);
    end_timing (SYS_getdents64, rc);
    check_retval ("getdents64", pentry->clock, pentry->retval, rc);
    if (rc > 0) {
	int compared = 0;
	char* p = dents; 
	char* c = tmpbuf;
	while (compared < rc) {
	    struct linux_dirent64* prev = (struct linux_dirent64 *) p;
	    struct linux_dirent64* curr = (struct linux_dirent64 *) c;
	    if (prev->d_ino != curr->d_ino || prev->d_off != curr->d_off ||
		prev->d_reclen != curr->d_reclen || prev->d_type != curr->d_type ||
		strcmp(prev->d_name, curr->d_name)) {
		printf ("{MISMATCH] getdetnts64: inode %llu vs. %llu\t", prev->d_ino, curr->d_ino);
		printf ("offset %lld vs. %lld\t", prev->d_off, curr->d_off);
		printf ("reclen %d vs. %d\t", prev->d_reclen, curr->d_reclen);
		printf ("name %s vs. %s\t", prev->d_name, curr->d_name);
		printf ("type %d vs. %d\n", prev->d_type, curr->d_type);
		handle_mismatch();
	    }
	    if (prev->d_reclen <= 0) break;
	    p += prev->d_reclen; c += curr->d_reclen; compared += prev->d_reclen;
	}
    }
    end_timing_func (SYS_getdents64);
    return rc;
}

long eventfd2_recheck ()
{
    struct recheck_entry* pentry;
    struct eventfd2_recheck* peventfd2;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    peventfd2 = (struct eventfd2_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("eventfd2: count %u flags %x rc %ld clock %lu\n", peventfd2->count, peventfd2->flags, pentry->retval, pentry->clock);
#endif 

    start_timing();
    rc = syscall(SYS_eventfd2, peventfd2->count, peventfd2->flags);
    end_timing(SYS_eventfd2, rc);
    check_retval ("eventfd2", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_eventfd2);
    return rc;
}

long poll_recheck (int timeout)
{
    struct recheck_entry* pentry;
    struct poll_recheck* ppoll;
    struct pollfd* fds;
    struct pollfd* pollbuf = (struct pollfd *) tmpbuf;
    short* revents;
    int rc, use_timeout;
    u_int i;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    ppoll = (struct poll_recheck *) bufptr;
    fds = (struct pollfd *) (bufptr + sizeof (struct poll_recheck));
    revents = (short *) (bufptr + sizeof (struct poll_recheck) + ppoll->nfds*sizeof(struct pollfd));
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("poll: buf %lx nfds %u timeout %d rc %ld", (u_long) ppoll->buf, ppoll->nfds, ppoll->timeout, pentry->retval);
    if (pentry->retval > 0) {
	for (i = 0; i < ppoll->nfds; i++) {
	    LPRINT ("\tfd %d events %x revents %x", fds[i].fd, fds[i].events, revents[i]);
	}
    }
    LPRINT (" clock %lu\n", pentry->clock);
#endif 

    if (ppoll->is_timeout_tainted) {
	use_timeout = timeout;
    } else {
	use_timeout = ppoll->timeout;
    }

    memcpy (tmpbuf, fds, ppoll->nfds*sizeof(struct pollfd));
    start_timing();
    rc = syscall(SYS_poll, pollbuf, ppoll->nfds, use_timeout);
    end_timing(SYS_poll, rc);
    if (rc > 0) {
	for (i = 0; i < ppoll->nfds; i++) {
	    LPRINT ("\tfd %d events %x returns revents %x\n", pollbuf[i].fd, pollbuf[i].events, pollbuf[i].revents);
	}
    }
    check_retval ("poll", pentry->clock, pentry->retval, rc);
    if (rc > 0) {
	for (i = 0; i < ppoll->nfds; i++) {
	    if (pollbuf[i].revents != revents[i]) {
		fprintf (stderr, "[MISMATCH] poll index %d: fd %d revents returns 0x%x expected 0x%x\t", i, fds[i].fd, pollbuf[i].revents, revents[i]);
		handle_mismatch();
	    }
	}
    }
    end_timing_func (SYS_poll);
    return rc;
}

long newselect_recheck ()
{
    struct recheck_entry* pentry;
    struct newselect_recheck* pnewselect;
    fd_set* readfds = NULL;
    fd_set* writefds = NULL;
    fd_set* exceptfds = NULL;
    struct timeval* use_timeout;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pnewselect = (struct newselect_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("newselect: nfds %d readfds %lx writefds %lx exceptfds %lx timeout %lx (tainted? %d) rc %ld clock %lu\n", pnewselect->nfds, (u_long) pnewselect->preadfds,
	    (u_long) pnewselect->pwritefds, (u_long) pnewselect->pexceptfds, (u_long) pnewselect->ptimeout, pnewselect->is_timeout_tainted, pentry->retval, pentry->clock);
#endif 

    if (pnewselect->preadfds) readfds = &pnewselect->readfds;
    if (pnewselect->pwritefds) readfds = &pnewselect->writefds;
    if (pnewselect->pexceptfds) readfds = &pnewselect->exceptfds;
    if (pnewselect->is_timeout_tainted) {
	use_timeout = pnewselect->ptimeout;
	LPRINT ("use_timeout is %lx %lx\n", pnewselect->ptimeout->tv_sec, pnewselect->ptimeout->tv_usec);
    } else {
	use_timeout = &pnewselect->timeout;
	LPRINT ("use_timeout is %lx %lx\n", pnewselect->timeout.tv_sec, pnewselect->timeout.tv_usec);
    }

    start_timing();
    rc = syscall(SYS__newselect, pnewselect->nfds, readfds, writefds, exceptfds, use_timeout);
    end_timing(SYS__newselect, rc);
    check_retval ("select", pentry->clock, pentry->retval, rc);
    if (readfds && memcmp (&pnewselect->readfds, readfds, pnewselect->setsize)) {
	printf ("[MISMATCH] select returns different readfds\n");
	handle_mismatch();
    }
    if (writefds && memcmp (&pnewselect->writefds, writefds, pnewselect->setsize)) {
	printf ("[MISMATCH] select returns different writefds\n");
	handle_mismatch();
    }
    if (exceptfds && memcmp (&pnewselect->exceptfds, exceptfds, pnewselect->setsize)) {
	printf ("[MISMATCH] select returns different exceptfds\n");
	handle_mismatch();
    }
    if (pnewselect->is_timeout_tainted) {
	add_to_taintbuf (pentry, NEWSELECT_TIMEOUT, use_timeout, sizeof(struct timeval));
    }
    end_timing_func (SYS__newselect);
    return rc;
}

long set_robust_list_recheck ()
{
    struct recheck_entry* pentry;
    struct set_robust_list_recheck* pset_robust_list;
    int rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pset_robust_list = (struct set_robust_list_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("set_robust_list: head %lx len %u rc %ld clock %lu\n", (u_long) pset_robust_list->head, pset_robust_list->len, pentry->retval, pentry->clock);
#endif 

    start_timing();
    rc = syscall(SYS_set_robust_list, pset_robust_list->head, pset_robust_list->len);
    end_timing(SYS_set_robust_list, rc);
    check_retval ("set_robust_list", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_set_robust_list);
    return rc;
}

long set_tid_address_recheck ()
{
    struct recheck_entry* pentry;
    struct set_tid_address_recheck* pset_tid_address;
    long rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pset_tid_address = (struct set_tid_address_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("set_tid_address: tidptr %lx rc %ld clock %lu\n", (u_long) pset_tid_address->tidptr, pentry->retval, pentry->clock);
#endif 

    start_timing();
    rc = syscall(SYS_set_tid_address, pset_tid_address->tidptr); 
    end_timing(SYS_set_tid_address, rc);
    LPRINT ("set_tid_address returns %ld\n", rc);
    add_to_taintbuf (pentry, RETVAL, &rc, sizeof(rc));
    end_timing_func (SYS_set_tid_address);
    return rc;
}

long rt_sigaction_recheck ()
{
    struct recheck_entry* pentry;
    struct rt_sigaction_recheck* prt_sigaction;
    struct sigaction* pact = NULL;
    char* data;
    long rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    prt_sigaction = (struct rt_sigaction_recheck *) bufptr;
    data = bufptr+sizeof(struct rt_sigaction_recheck);
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("rt_sigaction: sig %d act %lx oact %lx sigsetsize %d rc %ld clock %lu\n", prt_sigaction->sig, (u_long) prt_sigaction->act, (u_long) prt_sigaction->oact, prt_sigaction->sigsetsize, pentry->retval, pentry->clock);
#endif 

    if (prt_sigaction->act) pact = (struct sigaction *) data;
    rc = syscall(SYS_rt_sigaction, prt_sigaction->sig, pact, /*prt_sigaction->oact*/NULL, prt_sigaction->sigsetsize); 
    start_timing();
    check_retval ("rt_sigaction", pentry->clock, pentry->retval, rc);
    end_timing(SYS_rt_sigaction, rc);
    if (prt_sigaction->oact && rc == 0) {
	//add_to_taintbuf (pentry, SIGACTION_ACTION, prt_sigaction->oact, 20);
    }
    end_timing_func (SYS_rt_sigaction);
    return rc;
}

long rt_sigprocmask_recheck ()
{
    struct recheck_entry* pentry;
    struct rt_sigprocmask_recheck* prt_sigprocmask;
    sigset_t* pset = NULL;
    sigset_t* poset = NULL;
    char* data;
    long rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    prt_sigprocmask = (struct rt_sigprocmask_recheck *) bufptr;
    data = bufptr+sizeof(struct rt_sigprocmask_recheck);
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ("rt_sigprocmask: how %d set %lx oset %lx sigsetsize %d rc %ld clock %lu\n", prt_sigprocmask->how, (u_long) prt_sigprocmask->set, 
	    (u_long) prt_sigprocmask->oset, prt_sigprocmask->sigsetsize, pentry->retval, pentry->clock);
    fflush (stdout);
#endif 

    if (prt_sigprocmask->set) pset = (sigset_t *) data;
    if (prt_sigprocmask->oset) poset = (sigset_t *) tmpbuf;
    start_timing();
    rc = syscall(SYS_rt_sigprocmask, prt_sigprocmask->how, pset, poset, prt_sigprocmask->sigsetsize); 
    end_timing(SYS_rt_sigprocmask, rc);
    check_retval ("rt_sigprocmask", pentry->clock, pentry->retval, rc);
    if (prt_sigprocmask->oset) {
	if (prt_sigprocmask->set) {
	    if (memcmp (tmpbuf, data+prt_sigprocmask->sigsetsize, prt_sigprocmask->sigsetsize)) {
		printf ("[MISMATCH] sigprocmask returns different values %llx instead of expected %llx\n", *(__u64*)tmpbuf, *(__u64*)(data + prt_sigprocmask->sigsetsize));
		handle_mismatch();
	    }
	} else {
	    if (memcmp (tmpbuf, data, prt_sigprocmask->sigsetsize)) {
		printf ("[MISMATCH] sigprocmask returns different values %llx instead of expected %llx (no set)\n", *(__u64*)tmpbuf, *(__u64*)data);
		handle_mismatch();
	    }
	}
    }
    end_timing_func (SYS_rt_sigprocmask);
    return rc;
}

long mkdir_recheck ()
{
    struct recheck_entry* pentry;
    struct mkdir_recheck* pmkdir;
    long rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pmkdir = (struct mkdir_recheck *) bufptr;
    char* fileName = bufptr+sizeof(struct mkdir_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "mkdir: filename %s mode %d", fileName, pmkdir->mode);
    LPRINT ( " rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif
    start_timing();
    rc = syscall(SYS_mkdir, fileName, pmkdir->mode);
    end_timing (SYS_mkdir, rc);
    check_retval ("mkdir", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_mkdir);
    return rc;
}

long unlink_recheck ()
{
    struct recheck_entry* pentry;
    struct unlink_recheck* punlink;
    long rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    punlink = (struct unlink_recheck *) bufptr;
    char* pathname = bufptr+sizeof(struct unlink_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "unlink: pathname %p %s rc %ld clock %lu\n", punlink->pathname, pathname, pentry->retval, pentry->clock);
#endif
    start_timing();
    rc = syscall(SYS_unlink, pathname);
    end_timing (SYS_unlink, rc);
    check_retval ("unlink", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_unlink);
    return rc;
}

long sched_getaffinity_recheck (int pid)
{
    struct recheck_entry* pentry;
    struct sched_getaffinity_recheck* psched;
    pid_t use_pid;
    long rc;

    start_timing_func ();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    psched = (struct sched_getaffinity_recheck *) bufptr;
    bufptr += pentry->len;
    
#ifdef PRINT_VALUES
    LPRINT ( "sched_getaffinity: pid tainted? %d record pid %d passed pid %d clock %lu\n", 
	     psched->is_pid_tainted, psched->pid, pid, pentry->clock);
#endif 
    if (psched->is_pid_tainted) {
	use_pid = pid; 
    } else {
	use_pid = psched->pid;
    }

    start_timing();
    rc = syscall(SYS_sched_getaffinity, use_pid, psched->cpusetsize, tmpbuf);
    end_timing(SYS_sched_getaffinity, rc);
    check_retval ("sched_getaffinity", pentry->clock, pentry->retval, rc);
    if (rc == 0) {
        if (memcmp (tmpbuf, psched->mask, psched->cpusetsize)) {
            printf ("[MISMATCH] sched_getaffinity returns different cpu mask.\n");
            handle_mismatch ();
        }
    }
    end_timing_func (SYS_sched_getaffinity);
    return rc;
}

int ftruncate_recheck ()
{
    struct recheck_entry* pentry;
    struct ftruncate_recheck* pftruncate;
    long rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pftruncate = (struct ftruncate_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ( "ftruncate: fd %u length %lu", pftruncate->fd, pftruncate->length);
    LPRINT ( " rc %ld clock %lu\n", pentry->retval, pentry->clock);
#endif
    start_timing();
    rc = syscall(SYS_ftruncate, pftruncate->fd, pftruncate->length);
    end_timing (SYS_ftruncate, rc);
    check_retval ("ftruncate", pentry->clock, pentry->retval, rc);
    end_timing_func (SYS_ftruncate);
    return rc;
}

long prctl_recheck ()
{
    struct recheck_entry* pentry;
    struct prctl_recheck* pprctl;
    char* params;
    long rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    pprctl = (struct prctl_recheck *) bufptr;
    params = bufptr + sizeof(struct prctl_recheck);
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("prctl: option %d arg2 %lu arg3 %lu arg4 %lu arg5 %lurc %ld clock %lu\n", 
	    pprctl->option, pprctl->arg2, pprctl->arg3, pprctl->arg4, pprctl->arg5, pentry->retval, pentry->clock);
#endif
    start_timing();
    if (pprctl->option == PR_SET_NAME) {
	rc = syscall(SYS_prctl, pprctl->option, params);
    } else {
	rc = syscall(SYS_prctl, pprctl->option, pprctl->arg2, pprctl->arg3, pprctl->arg4, pprctl->arg5);
    }
    end_timing (SYS_prctl, rc);
    check_retval ("prctl", pentry->clock, pentry->retval, rc);
    if (pprctl->option == PR_GET_NAME) {
	if (!memcmp(params, (char *)pprctl->arg2, 16)) {
	    fprintf (stderr, "prctl getname returns name %16s instead of %16s\n", (char *) pprctl->arg2, params);
	    handle_mismatch ();
	}
    }
    end_timing_func (SYS_prctl);
    return rc;
}

long pipe_recheck ()
{
    struct recheck_entry* pentry;
    struct pipe_recheck* ppipe;
    long rc;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    ppipe = (struct pipe_recheck *) bufptr;
    bufptr += pentry->len;

#ifdef PRINT_VALUES
    LPRINT ("pipe: pipe %p values %d %d rc %ld clock %lu\n", 
	    ppipe->pipefd, ppipe->piperet[0], ppipe->piperet[1], pentry->retval, pentry->clock);
#endif
    start_timing();
    rc = syscall(SYS_pipe, ppipe->pipefd);
    end_timing (SYS_pipe, rc);
    check_retval ("pipe", pentry->clock, pentry->retval, rc);
    if (rc == 0) {
	if (ppipe->pipefd[0] != ppipe->piperet[0] || ppipe->pipefd[1] != ppipe->piperet[1]) {
	    fprintf (stderr, "pipe: received fds %d %d vs. exepcted %d %d\n", ppipe->pipefd[0], ppipe->pipefd[1], ppipe->piperet[0], ppipe->piperet[1]);
	    handle_mismatch();
	}
    }
    end_timing_func (SYS_pipe);
    return rc;
}

void recheck_wait_init ()
{
    if (go_live_clock) {
#ifdef PRINT_SCHEDULING
        int pid = syscall(SYS_gettid);
        printf ("Pid %d recheck_wait_clock_init: %lu mutex %p\n", pid, go_live_clock->slice_clock, &go_live_clock->mutex);
#endif
        go_live_clock->mutex = 0;
        __sync_sub_and_fetch (&go_live_clock->wait_for_other_threads, 1);
        while (go_live_clock->wait_for_other_threads) { 
            //wait until all threads are ready for slice execution
            //just use busy waiting here as it shouldn't take a long time
        }
#ifdef PRINT_SCHEDULING
        printf ("Pid %d recheck_wait_clock_init: all threads are ready to continue!\n", pid);
#endif
    }
}

void recheck_wait_proc_init ()
{
    if (go_live_clock) { 
#ifdef PRINT_SCHEDULING
        int pid = syscall (SYS_gettid);
        printf ("Pid %d recheck_wait_clock_proc_init: this thread is ready to continue\n", pid);
#endif
        __sync_sub_and_fetch (&go_live_clock->wait_for_other_threads, 1);
    }
}

void recheck_thread_wait (int record_pid)
{
    if (go_live_clock) {
        struct go_live_process_map* process_map = go_live_clock->process_map;
        int i = 0;
        struct go_live_process_map* p = NULL;
        int value = 0;
        int fail = 1;
#ifdef PRINT_SCHEDULING
        int pid = syscall(SYS_gettid);
        int actual_pid = 0;
#endif
        while (i < MAX_THREAD_NUM) {
            if (record_pid == process_map[i].record_pid) {
                p = &process_map[i];
                fail = 0;
#ifdef PRINT_SCHEDULING
                actual_pid = process_map[i].current_pid;
#endif
                break;
            }
            if (!process_map[i].record_pid) break;
            ++i;
        }
        if (fail) fprintf (stderr, "recheck_thread_wait cannot find the record_pid????\n");
#ifdef PRINT_SCHEDULING
        printf ("Pid %d call recheck_thread_wait, record_pid %d, addr %p, actual pid %d.\n", pid, record_pid, &p->wait, actual_pid);
        fflush (stdout);
#endif
        value = __sync_sub_and_fetch (&p->value, 1); 
        if (value < 0) {
            syscall (SYS_futex, &p->wait, FUTEX_WAIT, p->wait, NULL, NULL, 0);
        }
    }
}

void recheck_thread_wakeup (int record_pid)
{
    if (go_live_clock) {
        struct go_live_process_map* process_map = go_live_clock->process_map;
        int i = 0;
        struct go_live_process_map* p = NULL;
        int fail = 1;
        int value = 0;
#ifdef PRINT_SCHEDULING
        int actual_pid = 0;
        int pid = syscall(SYS_gettid);
#endif
        while (i < MAX_THREAD_NUM) {
            if (record_pid == process_map[i].record_pid) {
                p = &process_map[i];
#ifdef PRINT_SCHEDULING
                actual_pid = process_map[i].current_pid;
#endif
                fail = 0;
                break;
            }
            if (!process_map[i].record_pid) break;
            ++i;
        }
        if (fail) fprintf (stderr, "recheck_thread_wakeup cannot find the record_pid????\n");
#ifdef PRINT_SCHEDULING
        printf ("Pid %d call recheck_thread_wakeup, to wakeup %d (record_pid), %d (actual pid), addr %p\n", pid, record_pid, actual_pid, &p->wait);
        fflush (stdout);
#endif
        value = __sync_add_and_fetch (&p->value, 1);
        if (value <=0) {
            while (syscall (SYS_futex, &p->wait, FUTEX_WAKE, 1, NULL, NULL, 0) < 1)
                ;
        }
    }
}

int recheck_fake_clone (pid_t record_pid, pid_t* ptid, pid_t* ctid) 
{
    struct recheck_entry* pentry;

    start_timing_func();
    pentry = (struct recheck_entry *) bufptr;
    bufptr += sizeof(struct recheck_entry);
    last_clock = pentry->clock;
    bufptr += pentry->len;

    if (go_live_clock) {
        struct go_live_process_map* process_map = go_live_clock->process_map;
        int i = 0;
        pid_t ret = 0;
        int fail = 1;
        while (i < MAX_THREAD_NUM) {
            if (record_pid == process_map[i].record_pid) {
                ret = process_map[i].current_pid;
                fail = 0;
                break;
            }
            if (!process_map[i].record_pid) break;
            ++i;
        }
        if (fail) fprintf (stderr, "recheck_fake_clone cannot find the record_pid????\n");
#ifdef PRINT_VALUES
        LPRINT ("Pid %ld fake_clone ptid %p(original value %d), ctid %p(original value %d), record pid %d, children pid %d clock %ld\n", syscall(SYS_gettid), ptid, *ptid, ctid, *ctid, record_pid, ret, pentry->clock);
#endif
	// JNF - XXX - really should only do this if the appropriate flags are set 
        *ptid = ret;
        *ctid = ret;
#ifdef PRINT_VALUES
        LPRINT ("fake_clone ptid now has value %d, ctid %d\n", *ptid, *ctid);
#endif
	add_to_taintbuf (pentry, RETVAL, &ret, sizeof(long));
        return ret;
    } else 
        return 0;
}
