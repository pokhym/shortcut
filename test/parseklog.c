#include "parseklib.h"

#include <stdlib.h>
#include <stdio.h>

#include <unistd.h>
#include <getopt.h>

#include <assert.h>

int startup_ignore_flag = 1;
int startup_log_fd = -1;
int recheck_fd = -1;
int generate_startup = 0;
struct startup_entry {
	int sysnum;
	int len;
};
struct recheck_entry { 
	int sysnum;
	int flag;
	long retval;
	int len;
};
static void empty_printfcn(FILE *out, struct klog_result *res) {
}

static void startup_default_printfcn (FILE* out, struct klog_result* res) { 
	int rc = 0;
	struct startup_entry entry;
	void* buf = NULL;

	default_printfcn (out, res);
	//read from startup log
	if (startup_ignore_flag == 0) { 
		rc = read (startup_log_fd, &entry, sizeof(entry));
		if (rc == 0) {
			//end of startup log; do nothing
			exit (EXIT_SUCCESS);
		}
		assert (rc == sizeof(entry));
		if (entry.len > 0) {
			buf = malloc (entry.len);
			rc = read (startup_log_fd, buf, entry.len);
		}
		startup_ignore_flag = 0;
		//check syscall
		if (res->psr.sysnum != entry.sysnum) { 
			fprintf (stderr, "MISMATCH syscall, expected %d in the reply log, while in the startup log %d\n", res->psr.sysnum, entry.sysnum);
			exit (EXIT_FAILURE);
		}
		res->startup_retparams = buf;
		res->startup_retsize = entry.len;
	} else { 
		startup_ignore_flag = 0; 
		//TODO:this is to ignroe the first execve
	}
}

static void write_header_into_recheck_log (int sysnum, long retval, int flag, int len) { 
	int rc = 0;
	struct recheck_entry entry;
	entry.sysnum = sysnum;
	entry.len = (len>0?len:0);
	entry.flag = flag;
	entry.retval = retval;
	if (recheck_fd <= 0) { 
		exit(EXIT_FAILURE);
	}
	rc = write (recheck_fd, (void*) &entry, sizeof(struct recheck_entry));
	if (rc != sizeof(struct recheck_entry)) { 
		assert (0 && "cannot write into startup");
	}
	fprintf (stderr, "write_header_into_recheck_log: len %d\n", len);
}
static void write_content_into_recheck_log (void* buf, int len) {
	int rc = 0;
	/*rc = write (recheck_fd, &len, sizeof (len));
	assert (rc == sizeof(len));*/
	rc = write (recheck_fd, buf, len);
	assert (rc == len); 
}

static void print_write_pipe(FILE *out, struct klog_result *res) {
	char *retparams = res->retparams;
	int *is_shared = (int *)retparams;

	if (retparams && *is_shared != NORMAL_FILE) {
		int id = *(is_shared+1);
		fprintf(out, "%d, %ld, %lu, %lld\n", id, res->retval,
				res->start_clock, res->index);
	}
}

static void print_socketcall_pipe(FILE *out, struct klog_result *res) {
	char *retparams = res->retparams;

	if (retparams) {
		u_int shared;
		int call = *((int *)retparams);
		retparams += sizeof(int);

		switch (call) {
			case SYS_SEND:
			case SYS_SENDTO:
				shared = *((u_int *)retparams);
				if (shared & IS_PIPE) {
					int pipe_id;

					pipe_id = *((int *)retparams);
					retparams += sizeof(int);

					fprintf(out, "%d, %ld, %lu, %lld\n", pipe_id, res->retval,
							res->start_clock, res->index);
					break;
				}
		}
	}
}

static void do_print_graph(FILE *out, struct klog_result *res, char *retparams,
		u_int is_cached) {
	int i;

	if (is_cached & CACHE_MASK) {
		struct replayfs_filemap_entry *entry;
		entry = (struct replayfs_filemap_entry *)retparams;

		for (i = 0; i < entry->num_elms; i++) {
			fprintf(out, "%lld %lld %d {%lld, %d, %lld, %d, %d}\n",
					res->index, entry->elms[i].offset - entry->elms[0].offset, entry->elms[i].size,
					(loff_t)entry->elms[i].bval.id.unique_id, entry->elms[i].bval.id.pid,
					(loff_t)entry->elms[i].bval.id.sysnum,
					entry->elms[i].read_offset, entry->elms[i].size);
		}
	} else if (is_cached & IS_PIPE_WITH_DATA) {
		struct replayfs_filemap_entry *entry;
		entry = (struct replayfs_filemap_entry *)retparams;


		for (i = 0; i < entry->num_elms; i++) {
			fprintf(out, "pipe: %lld %d %d {%lld, %d, %lld, %d, %ld}\n",
					res->index, entry->elms[i].bval.buff_offs, entry->elms[i].size,
					(loff_t)entry->elms[i].bval.id.unique_id, entry->elms[i].bval.id.pid,
					(loff_t)entry->elms[i].bval.id.sysnum,
					entry->elms[i].read_offset, res->retval);
		}
	} else if (is_cached & IS_PIPE) {
		uint64_t writer;
		int pipe_id;
		writer = *((uint64_t *)retparams);
		retparams += sizeof(uint64_t);
		pipe_id = *((int *)retparams);
		fprintf(out, "pipe: %lld, %d, %lld {%ld} {%lu}\n", writer, pipe_id,
				res->index, res->retval, res->start_clock);
	}
}

static void print_socketcall_graph(FILE *out, struct klog_result *res) {
	char *retparams = res->retparams;

	if (retparams) {
		int call = *((int *)retparams);
		u_int is_cached;
		retparams += sizeof(int);

		switch (call) {
			case SYS_RECV:
				retparams += sizeof(struct recvfrom_retvals) - sizeof(int) + res->retval;
				is_cached = *((u_int *)retparams);
				is_cached += sizeof(u_int);
				do_print_graph(out, res, retparams, is_cached);
				break;
			case SYS_RECVFROM:
				retparams += sizeof(struct recvfrom_retvals) - sizeof(int) + res->retval-1; 
				is_cached = *((u_int *)retparams);
				is_cached += sizeof(u_int);
				do_print_graph(out, res, retparams, is_cached);
				break;
		}
	}
}

static void print_read_graph(FILE *out, struct klog_result *res) {
	char *retparams = res->retparams;

	if (retparams) {
		u_int is_cached;
		is_cached = *((u_int *)retparams);
		retparams += sizeof(u_int);

		if (is_cached) {
			/* Fast forward past offset */
			if (is_cached & CACHE_MASK) {
				retparams += sizeof (long long int);
			}
			do_print_graph(out, res, retparams, is_cached);
		}
	}
}

static void print_socketcall(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		int call = *((int *)res->retparams);
		fprintf(out, "         Socketcall is %d\n", call);
	}
}

static void print_rt_sigaction(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		struct sigaction* sa = (struct sigaction *)res->retparams;
		fprintf(out, "         sa handler is %lx\n", (unsigned long) sa->sa_handler);
	}
}

static void print_getcwd(FILE *out, struct klog_result *res) {

	parseklog_default_print(out, res);

	fprintf(out, "         path has %d bytes\n", res->retparams_size-sizeof(int));
	if (res->retparams_size-sizeof(int) > 0) {
		fprintf(out, "         path is %s\n", ((char *)res->retparams)+sizeof(int));
	}
}

static void print_clock_gettime(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		struct timespec* time = (struct timespec*)res->retparams;
		fprintf(out, "clock_gettime tv_sec:%ld, tv_nsec:%ld\n", time->tv_sec, time->tv_nsec);
	}
}

static void print_mmap(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		fprintf(out, "         dev is %lx\n",
				((struct mmap_pgoff_retvals *)res->retparams)->dev);
		fprintf(out, "         ino is %lx\n",
				((struct mmap_pgoff_retvals *)res->retparams)->ino);
		fprintf(out, "         mtime is %lx.%lx\n",
				((struct mmap_pgoff_retvals *)res->retparams)->mtime.tv_sec,
				((struct mmap_pgoff_retvals *)res->retparams)->mtime.tv_nsec);
	}
}

//here is the recheck heuristic:
//for READ ONLY files(should already be cached), recheck on open;
//otherwise, recheck on each read
struct open_params { 
	int flag;
	int mode;
	char filename[0];
};
static void print_open(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		struct open_retvals *oret = res->retparams;
		fprintf(out, "         Open dev is %lX, ino %lX\n", oret->dev, oret->ino);
	}
	if (generate_startup) { 
		//for read-only files, check mtime here instead of read; also re-open and check retval
		char* filename = (char*) (((struct open_params*)res->startup_retparams)->filename);
		if (psr->flags & SR_HAS_RETPARAMS) {
			struct open_retvals* or = res->retparams;
			write_header_into_recheck_log (5, res->retval, 0, sizeof (struct open_retvals) + res->startup_retsize);
			write_content_into_recheck_log (res->retparams, sizeof(struct open_retvals));
			printf ("     Open cache file filename %s, mtime %lu %lu\n", filename, or->mtime.tv_sec, or->mtime.tv_nsec);
		} else {
			write_header_into_recheck_log (5, res->retval, 1, res->startup_retsize);
		}
		if (filename != NULL) { 
			write_content_into_recheck_log (res->startup_retparams, res->startup_retsize);
		} else { 
			fprintf (stderr, "cannot parse filename for open? retsize %d\n", res->startup_retsize);
		}
	}
}

static void print_write(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		char *retparams = res->retparams;
		int *is_shared = (int *)retparams;

		if (*is_shared == NORMAL_FILE) {
			long long *id = (long long *)(is_shared+1);
			fprintf(out, "         With write_id of %lld\n", *id);
		} else {
			int *id = (int *)(is_shared+1);
			fprintf(out, "         Write is part of pipe: %d\n", *(id));
		}
	}
}

static void print_read(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		char *buf = res->retparams;

		int is_cache_read = *((int *)buf);

		if (is_cache_read & READ_NEW_CACHE_FILE) {
			struct open_retvals *orets = (void *)(buf + sizeof(int) + sizeof(loff_t));
			fprintf(out, "         Updating cache file to {%lu, %lu, (%ld, %ld)}",
					orets->dev, orets->ino, orets->mtime.tv_sec, orets->mtime.tv_nsec);
			if (generate_startup) fprintf (stderr, "Not handled for READ_NEW_CACHE_FILE\n");
		}

		if (is_cache_read & CACHE_MASK) {
			fprintf(out, "         offset is %llx\n", *((long long int *) buf));
#ifdef TRACE_READ_WRITE
				do {
					struct replayfs_filemap_entry *entry;
					int i;
					entry = (struct replayfs_filemap_entry *)(buf + sizeof(long long int));
					fprintf(out, "         Number of writes sourcing this read: %d\n",
							entry->num_elms);

					for (i = 0; i < entry->num_elms; i++) {
						fprintf(out, "         \tSource %d is {id, pid, syscall_num} {%lld %d %lld}\n", i,
								(loff_t)entry->elms[i].bval.id.unique_id, entry->elms[i].bval.id.pid,
								(loff_t)entry->elms[i].bval.id.sysnum);
					}
				} while (0);
#endif
#ifdef TRACE_PIPE_READ_WRITE
		} else if (is_cache_read & IS_PIPE) {
			if (is_cache_read & IS_PIPE_WITH_DATA) {
				struct replayfs_filemap_entry *entry;
				int i;
				/* Get data... */
				entry = (struct replayfs_filemap_entry *)(buf);
				fprintf(out, "         Piped writes sourcing this read: %d\n",
						entry->num_elms);

				for (i = 0; i < entry->num_elms; i++) {
					fprintf(out, "         \tSource %d is {id, pid, syscall_num} {%lld %d %lld}\n", i,
							(loff_t)entry->elms[i].bval.id.unique_id, entry->elms[i].bval.id.pid,
							(loff_t)entry->elms[i].bval.id.sysnum);
				}
			} else {
				fprintf(out, "         File is a pipe sourced by id %llu, pipe id %d\n",
						*((uint64_t *)buf), 
						/* Yeah, I went there */
						*((int *)((uint64_t *)buf + 1)));
			}
#endif
		}
	}
	if (generate_startup)  {
		int write_content = 0;
		if (psr->flags & SR_HAS_RETPARAMS) { 
			char* buf = res->retparams;
			int is_cache_read = *((int*)buf);

			if ((is_cache_read & CACHE_MASK) == 0) {
				//include all bytes into recheck log
				write_header_into_recheck_log (3, res->retval, 0, res->retval);
				write_content_into_recheck_log (res->retparams + sizeof(int), res->retval);
				write_content = 1;
			}
		}
		if (write_content == 0) { 
			write_header_into_recheck_log (3, res->retval, 1, 0);
		}
	}
}

static void print_close (FILE* out, struct klog_result* res) { 
	parseklog_default_print(out, res);
	if (generate_startup) {
		write_header_into_recheck_log (6, res->retval, 0, res->startup_retsize);
		write_content_into_recheck_log (res->startup_retparams, res->startup_retsize);
	}
}

static void print_access (FILE* out, struct klog_result* res) { 
	parseklog_default_print(out, res);
	if (generate_startup) { 
		write_header_into_recheck_log (33, res->retval, 0, res->startup_retsize);
		write_content_into_recheck_log (res->startup_retparams, res->startup_retsize);
	}
}

static void print_waitpid(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;
	int *buf = res->retparams;
	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		fprintf(out, "         Status is %d\n", *buf);
	}
}

static void print_pipe(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;
	int *buf = res->retparams;

	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		fprintf(out, "         pipe returns (%d,%d)\n", buf[0], buf[1]);
	}
}

static void print_gettimeofday(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;
	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		struct gettimeofday_retvals* gttd =
			(struct gettimeofday_retvals*) res->retparams;
		fprintf(out, "         gettimeofday has_tv %d, has_tz %d, tv_sec %ld, tv_usec %ld\n", 
				gttd->has_tv, gttd->has_tz, gttd->tv.tv_sec, gttd->tv.tv_usec);
	}
}

static void print_stat(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;
	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		struct stat64* pst = (struct stat64 *) res->retparams;

		fprintf(out, "         stat64 size %Ld blksize %lx blocks %Ld ino %Ld\n", 
			pst->st_size, pst->st_blksize, pst->st_blocks, pst->st_ino);
	}
}

static void print_epoll_wait(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;
	parseklog_default_print(out, res);

	if (psr->flags & SR_HAS_RETPARAMS) {
		long i;
		for (i = 0; i < res->retval; i++) {
			struct epoll_event* pe = (struct epoll_event *) res->retparams;
			fprintf(out, "         epoll_event flags %x data %p\n", 
				pe[i].events, pe[i].data.ptr);
		}
	}
}

static void print_execve(FILE *out, struct klog_result *res) {
	struct syscall_result *psr = &res->psr;

	parseklog_default_print(out, res);

	if ((psr->flags & SR_HAS_RETPARAMS) != 0) {
		struct execve_retvals* per = (struct execve_retvals *) res->retparams;
		if (per->is_new_group) {
			fprintf(out, "\tnew group id: %lld\n", per->data.new_group.log_id);
		} else {
			int i;
			fprintf(out, "\tnumber of random values is %d\n", per->data.same_group.rvalues.cnt);
			for (i = 0; i < per->data.same_group.rvalues.cnt; i++) {
				fprintf(out, "\t\trandom values %d is %lx\n", i, per->data.same_group.rvalues.val[i]);
			}
			fprintf(out, "\tdev is %lx\n", per->data.same_group.dev);
			fprintf(out, "\tino is %lx\n", per->data.same_group.ino);
			fprintf(out, "\tmtime is %lx.%lx\n", per->data.same_group.mtime.tv_sec, per->data.same_group.mtime.tv_nsec);
			fprintf(out, "\tuid is %d\n", per->data.same_group.evalues.uid);
			fprintf(out, "\teuid is %d\n", per->data.same_group.evalues.euid);
			fprintf(out, "\tgid is %d\n", per->data.same_group.evalues.gid);
			fprintf(out, "\tegid is %d\n", per->data.same_group.evalues.egid);
			fprintf(out, "\tAT_SECURE is %d\n", per->data.same_group.evalues.secureexec);
		}
	}
}

enum printtype {
	BASE = 0,
	PIPE,
	GRAPH
};

void print_usage(FILE *out, char *progname) {
	fprintf(out, "Usage: %s [-p] [-g] [-h] [-s] logfile\n", progname);
}

void print_help(char *progname) {
	print_usage(stdout, progname);
	printf(" -h       Prints this dialog\n");
	printf(" -g       Only prints file graph information\n");
	printf(" -p       Only prints pipe write information\n");
	printf(" -s 	  generate startup caches.\n");
}

int main(int argc, char **argv) {
	struct klogfile *log;
	struct klog_result *res;
	char startup_filename[256];

	enum printtype type = BASE;

	int opt;
	unsigned long gid;
	int pid;

	while ((opt = getopt(argc, argv, "gphs")) != -1) {
		switch (opt) {
			case 'g':
				type = GRAPH;
				break;
			case 'p':
				type = PIPE;
				break;
			case 'h':
				print_help(argv[0]);
				exit(EXIT_SUCCESS);
			case 's':
				generate_startup = 1;
				break;
			default:
				print_usage(stderr, argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	if (argc - optind != 1) {
		print_usage(stderr, argv[0]);
		exit(EXIT_FAILURE);
	}

	log = parseklog_open(argv[optind]);
	//get the group id and log id (this requires the absolute path in the params) 
	if (strncmp (argv[optind], "/replay_logdb/rec_", 18) != 0) { 
		printf ("use absulte log path.\n");
		exit (EXIT_FAILURE);
	} else { 
		char* index = strrchr (argv[optind], '/');
		gid = atol (argv[optind] + 18);
		pid = atoi (index + 9);
	}
	printf ("Group id %lu, pid %d\n", gid, pid);
	//open startup log
	if (generate_startup) { 
		sprintf (startup_filename, "/startup_db/%lu/startup.%d", gid, pid);
		startup_log_fd = open (startup_filename, O_RDONLY);
		if (startup_log_fd < 0) { 
			printf ("cannot open startup log, ret %d, filename %s\n", startup_log_fd, startup_filename);
			exit (EXIT_FAILURE);
		}
		sprintf (startup_filename, "/startup_db/%lu/startup.%d.recheck", gid, pid);
		recheck_fd = open (startup_filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
		if (recheck_fd < 0) {
			printf ("cannot open startup log, ret %d, filename %s\n", startup_log_fd, startup_filename);
			exit (EXIT_FAILURE);
		}
	}
	if (!log) {
		fprintf(stderr, "%s doesn't appear to be a valid log file!\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if (type == BASE) {
		parseklog_set_printfcn(log, print_read, 3);
		parseklog_set_printfcn(log, print_write, 4);
		parseklog_set_printfcn(log, print_open, 5);
		parseklog_set_printfcn(log, print_waitpid, 7);
		parseklog_set_printfcn(log, print_execve, 11);
		parseklog_set_printfcn(log, print_pipe, 42);
		parseklog_set_printfcn(log, print_gettimeofday, 78);
		parseklog_set_printfcn(log, print_socketcall, 102);
		parseklog_set_printfcn(log, print_write, 146);
		parseklog_set_printfcn(log, print_rt_sigaction, 174);
		parseklog_set_printfcn(log, print_getcwd, 182);
		parseklog_set_printfcn(log, print_mmap, 192);
		parseklog_set_printfcn(log, print_stat, 195);
		parseklog_set_printfcn(log, print_stat, 196);
		parseklog_set_printfcn(log, print_stat, 197);
		parseklog_set_printfcn(log, print_epoll_wait, 256);
		parseklog_set_printfcn(log, print_clock_gettime, 265);
		if (generate_startup){
			parseklog_set_default_printfcn (log, startup_default_printfcn);
			parseklog_set_printfcn(log, print_close, 6);
		}
	} else if (type == GRAPH) {
		parseklog_set_default_printfcn(log, empty_printfcn);
		parseklog_set_signalprint(log, empty_printfcn);

		parseklog_set_printfcn(log, print_read_graph, 3);
		parseklog_set_printfcn(log, print_socketcall_graph, 102);
	} else if (type == PIPE) {
		parseklog_set_default_printfcn(log, empty_printfcn);
		parseklog_set_signalprint(log, empty_printfcn);

		parseklog_set_printfcn(log, print_write_pipe, 4);
		parseklog_set_printfcn(log, print_write_pipe, 146);
		parseklog_set_printfcn(log, print_socketcall_pipe, 102);
	}

	while ((res = parseklog_get_next_psr(log)) != NULL) {
		//stop at the checkpoint clock
		klog_print(stdout, res);
	}

	parseklog_close(log);
	if (generate_startup) {
		close (startup_log_fd);
		close (recheck_fd);
	}

	return EXIT_SUCCESS;
}

