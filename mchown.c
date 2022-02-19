/*
 * Copyright 2020-2022 Andrew Sharp andy@tigerand.com, All Rights Reserved
 */

/*
 * the mainline code and data structures for multi-threaded super-chown
 */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pwd.h>
#include <grp.h>

#include "mchown.h"
#ifdef MDEBUG
int debug = MDEBUG;
#endif

struct dir_job *dir_jobs;        /* array of dir_jobs of size nthreads+1 */
struct dir_job *dj_freelist;     /* pointer to top of list of free dirjobs */
pthread_mutex_t queue_lock;
pthread_cond_t queue_cv;

int nthreads;                    /* the number of pool threads we have */
//int n_avail_threads;            /* number of sleeping threads - queue_lock */
int dname_max;                   /* the size to allocate for dirent struct */
uint64_t files_chowned;
uint64_t dirs_chowned;

int enqueue(char *dpath, char *name, struct creds *creds, uint64_t dj_id);

#define SYS_CPU_FILE "/sys/devices/system/cpu/online"
#define DJ_PATH_SZ 2048          /* the number of bytes to allocate for the
                                  * string pointed to by dirjob->path */

 int
get_core_count(void)
{
	int fd;
	char online[512];
	int c_hores;
	int chars_read;
	char *tok;           /* token pointer */
	char *prog;          /* progress pointer */
	int begin_cpu;       /* the cpu num at the beginning of a range */
	int end_cpu;         /* the cpu num at the end of a range */

	online[0] = '\0';
	c_hores = 0;       /* we know there has to be at least one ... */

	fd = open(SYS_CPU_FILE, O_RDONLY);
	if (fd < 0) {
		FERR("Problem opening %s for core count.  errno = %d", SYS_CPU_FILE,
			errno);
		return 0;
	}

	chars_read = read(fd, &online[0], sizeof(online) - 1);
	if (chars_read < 1) {
		FERR("Problem reading %s for core count.  errno = %d", SYS_CPU_FILE,
			errno);
		return 0;
	}
	online[chars_read] = '\0';

	/*
	 * what a holey pain in the
	 */
	prog = &online[0];
	while ((tok = strpbrk(prog, ",-")) != NULL) {
		if (*tok == ',') {
			c_hores++;
		} else if (*tok == '-') {
			*tok = '\0';
			sscanf(prog, "%d", &begin_cpu);
			prog = tok + 1;
			tok = strpbrk(prog, ",");
			if (tok) {
				*tok = '\0';
			}
			sscanf(prog, "%d", &end_cpu);
			c_hores = c_hores + (end_cpu - begin_cpu + 1);
			if (tok == NULL) {
				prog = tok;
				break;
			}
		}
		prog = tok + 1;
	}
	if (prog && sscanf(prog, "%d", &end_cpu) == 1) {
		c_hores++;
	}

	return c_hores;
}


/*
 * create the dirid for a path/credential set
 * simple incrementing ID for now.  some kind of 64 bit hash in the future?
 */
 uint64_t
mk_dirid(char *path __attribute__ ((unused)),
	struct creds *cred __attribute__ ((unused)))
{
	static uint64_t  dindex = 1;

	MBUG("mk_dirid: returning new dir_id %lu", dindex);
	return dindex++;
}


struct creds {
	uid_t u;
	gid_t g;
} *cred_tbl;

/*
 * find an unused slot in creds array
 */
 struct creds *
get_cred(uid_t uid, gid_t gid)
{
	int i;

	MBUG(" get_cred nthreads=%d, cred_tbl=%p, uid=%d, gid=%d", nthreads,
		cred_tbl, uid, gid);
	for (i = 0; i <= nthreads; i++) {
		if ((cred_tbl[i].u == uid) && (cred_tbl[i].g == gid)) {
			MBUG(" matching ids cred slot found @ %p index %d", &cred_tbl[i],
				i);
			return &cred_tbl[i];
		}
	}

	/*
     * this is a credential set we don't already have, so find a slot for it
	 */
	for (i = 0; i <= nthreads; i++) {
		if ((cred_tbl[i].u == (uid_t)-1) && (cred_tbl[i].g == (gid_t)-1)) {
			cred_tbl[i].u = uid;
			cred_tbl[i].g = gid;
			MBUG(" empty cred slot found @ %p index %d", &cred_tbl[i], i);
			return &cred_tbl[i];
		}
		MBUG(" cred_tbl[%d].u=%d, cred_tbl[%d].g=%d", i, cred_tbl[i].u, i,
			cred_tbl[i].g);
		break;
	}

	/*
	 * this should never be reached
	 */
	MBUG(" no cred slot found");
	return NULL;
}


/*
 * return a cred slot back to unused status
 */
 void
rel_cred(struct creds *cr)
{
	cr->u = (uid_t)-1;
	cr->g = (gid_t)-1;
}


#define is_dir(DENTRY) (DENTRY->d_type == DT_DIR)
#define is_reg(DENTRY) (DENTRY->d_type == DT_REG)
#define is_lnk(DENTRY) (DENTRY->d_type == DT_LNK)
#define is_dir_or_reg(DENTRY) ((is_dir(DENTRY) || is_reg(DENTRY) || \
	is_lnk(DENTRY)))


/*
 * change the uid/gid for a regular file
 *
 * possibly inline this, or just make it a macro
 */
 int
chown_reg(int dir_fd, char *dname, struct stat *statbuf, struct creds *cred)
{
	int rval;

	rval = 0;

	/* should not get here if file is symlink ... */
	rval = fstatat(dir_fd, dname, statbuf, AT_SYMLINK_NOFOLLOW);
		/* must define __USE_GNU before include fcntl.h to use
		 * AT_NO_AUTOMOUNT flag
		 * AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT */
	if (rval) {
		return -1;
	}
	if ((statbuf->st_uid != cred->u) || (statbuf->st_gid != cred->g)) {
		rval = fchownat(dir_fd, dname, cred->u, cred->g, AT_SYMLINK_NOFOLLOW);
		if (rval) {
			return -2;
		}
	} else {
		return -3;
	}

	return rval;
}


/*
 * build a new dir_job structure
 */
#define MK_DIRJOB(DJ, MDJ, BPATH, DENTRY) {                                 \
				DJ = *MDJ;                                                  \
				(DJ).path = BPATH;                                          \
				strncpy((DJ).path, MDJ->path, DJ_PATH_SZ - 1);              \
				strncat((DJ).path, "/", 2);                                 \
				strncat((DJ).path, DENTRY->d_name, DJ_PATH_SZ - 1);}


/*
 * main directory processing function
 */
 int
mdpf(struct dir_job *my_dirjob)
{
	DIR *dirptr;
	struct creds *creds;
	char bpath[DJ_PATH_SZ];	/* used for constructing file names for use in
							 * the NEXT call to mdpf */
	char m_err_str[128];
	int myfd;
	struct dirent *dentry;
	struct dirent *res_dentry;
	struct dirent *s_dentry;
	struct stat statbuf;
	int rd_status;
	int rval;
	int dirs_queued;
	int reg_procd;
	int dir_procd;
	int lnk_procd;
	int ndentries;				/* the number of directory entries that we
								 * find interesting */
	struct dir_job s_dir_job;   /* for single threaded operation */
	struct dir_job *ldjob;      /* for single threaded operation */


	dirs_queued = reg_procd = lnk_procd = dir_procd = 0;
	creds = my_dirjob->ucred;   /* just cache this as we use it a lot */

	MBUG(" mdpf called with my_dirjob=%p path '%s' uid %d gid %d", my_dirjob,
		my_dirjob->path, creds->u, creds->g);

	/* open the dir and start reading the entries */
	dirptr = opendir(my_dirjob->path);
	if (dirptr == NULL) {
		rval = errno;
		strerror_r(rval, m_err_str, 128);
		FERR("[%02d] mdpf: opendir failed on called dir '%s' errno %d - %s",
			my_tpool->thread_num, my_dirjob->path, rval, m_err_str);
		shutdown_time++;
		//sleep(10);
		return -1;
	}

	/* get the fd from the DIR* */
	myfd = dirfd(dirptr);

	/* allocate the dirent for dentry and save_dentry structures */
	dentry = calloc(1, (size_t)dname_max);
	if (dentry == NULL) {
		FERR("[%02d] Failed allocating dentry errno = %d", my_tpool->thread_num,
			errno);
		closedir(dirptr);
		return -1;
	}
	s_dentry = calloc(1, (size_t)dname_max);
	if (s_dentry == NULL) {
		FERR("[%02d] Failed allocating s_dentry errno = %d",
			my_tpool->thread_num, errno);
		free(dentry);
		closedir(dirptr);
		return -1;
	}
	MBUG("dentry and s_dentry allocated with size %d bytes", dname_max);

	/*
	 * process this directory
	 */
	if(fstat(myfd, &statbuf)) {
		FERR("Failed to stat '%s' errno %d", my_dirjob->path, errno);
		closedir(dirptr);
		free(dentry);
		free(s_dentry);
		return -1;
	}
	if ((statbuf.st_uid != creds->u) || (statbuf.st_gid != creds->g)) {
		if(fchown(myfd, creds->u, creds->g)) {
			FERR("Failed to process this '%s' dir errno %d", my_dirjob->path,
				errno);
			closedir(dirptr);
			free(dentry);
			free(s_dentry);
			return -1;
		}
		dir_procd++;
		MBUG("processed this '%s' dir", my_dirjob->path);
	} else {
		MBUG("this '%s' dir already the desired owner", my_dirjob->path);
	}

	/*
	 * process the files in this directory
	 */
	ndentries = 0;
	rval = 0;
	while ((!shutdown_time) && (!rval)) { /* stop loop if shutdown */
		rd_status = readdir_r(dirptr, dentry, &res_dentry);

		if (rd_status != 0) {
			rval = errno;
			strerror_r(rval, m_err_str, 128);
			FERR("readdir_r returned %d errno %d - %s", rd_status, rval,
				m_err_str);
			break;
		}
		if (res_dentry == NULL) { /* normal EOD state */
			break;
		}

		/* we only care about directories, regular files, and symlinks */
		if (!is_dir_or_reg(dentry)) {
			continue;
		}
		/* ignore "." and ".." entries */
		if ((!strncmp(dentry->d_name, ".", 2)) ||
			(!strncmp(dentry->d_name, "..", 3))) {

			continue;
		}

		ndentries = ndentries + 1;

		if (is_reg(dentry) || is_lnk(dentry)) {
			if (is_reg(dentry)) {
				MBUG("chowning reg file '%s'", dentry->d_name);
			} else {
				MBUG("chowning lnk file '%s'", dentry->d_name);
			}
			rval = chown_reg(myfd, dentry->d_name, &statbuf, creds);
			switch (rval) {
				case -1:
					rval = errno;
					FERR("Failed stat of '%s' errno = %d", dentry->d_name,
						errno);
					break;
				case -2:
					rval = errno;
					FERR("Failed chown of '%s' errno = %d", dentry->d_name,
						errno);
					break;
				case -3:
					rval = 0;
					MBUG(" reg file '%s' already the desired owner",
						dentry->d_name);
					break;
				case 0:
					if (is_reg(dentry)) {
						reg_procd++;
					} else {
						lnk_procd++;
					}
					break;
			}
		} else if (is_dir(dentry)) {
			/* process the first directory outside the loop ... */
			if (ndentries == 1) {
				MBUG("mdpf - delay processing of %s/%s", my_dirjob->path,
					dentry->d_name);
				*s_dentry = *dentry;  /* this is a structure copy */
				continue;
			}

			/* process a directory in the normal loop path */
			MBUG("calling in-loop enqueue with path '%s' dentry '%s'",
				my_dirjob->path, dentry->d_name);
			if (!enqueue(my_dirjob->path, &dentry->d_name[0], creds,
				my_dirjob->job_id)) {

				/*
				 * this block is the in-loop single-thread path
				 */

				/* if in shutdown, avoid calling mdpf again */
				if (shutdown_time) {
					MBUG(" enqueue returned nak - in shutdown state");
					break;
				}

				MBUG(" enqueue nak, dropping to single thread");
				MK_DIRJOB(s_dir_job, my_dirjob, bpath, dentry);
				rval = mdpf(&s_dir_job);
				TZERO_DJ(&s_dir_job);
			} else {
				dirs_queued++;
			}
		}
	}

	closedir(dirptr);

	if (shutdown_time) {
		MBUG(" mdpf - shutdown_time set %d", shutdown_time);
	}

	if ((rd_status == 0) && (res_dentry == NULL) && (!shutdown_time) &&
		(!rval)) {

		if (is_dir(s_dentry)) {
			if (ndentries > 1) {
				/* process the first directory in the out-of-loop path */
				if (enqueue(my_dirjob->path, &s_dentry->d_name[0], creds,
					my_dirjob->job_id)) {

					dirs_queued++;
					goto mdpf_exit; /* if last dir is queued, then done */
				} else {
					MBUG(" mdpf - enqueue nak, dropping to single thread");
				}
			} else { /* ndentries == 1 */
				/* recurse into the 1-and-only directory */
				MBUG("delayed single entry directory '%s', recursing into '%s'",
					my_dirjob->path, s_dentry->d_name);
			}

			/*
			 * here we recurse into the last/only directory, either because
			 * it's the only processable file in the directory, or because
			 * enqueue above failed
			 */
			MBUG("strlen my_djob->path = %lu", strlen(my_dirjob->path));
			MBUG("strlen s_dentry->d_name = %lu", strlen(s_dentry->d_name));
			MK_DIRJOB(s_dir_job, my_dirjob, bpath, s_dentry);
			MBUG("calling mdpf with path '%s'", s_dir_job.path);
			rval = mdpf(&s_dir_job);
		}
	}

mdpf_exit:

	free(dentry);
	free(s_dentry);

	MBUG("%s: files processed: %d, links processed: %d, dirs processed: %d, dirs queued: %d",
		my_dirjob->path, reg_procd, lnk_procd, dir_procd, dirs_queued);

	__sync_add_and_fetch(&files_chowned, reg_procd + lnk_procd, NULL);
	__sync_add_and_fetch(&dirs_chowned, dir_procd, NULL);

	MBUG("mdpf returning rval=%d", rval);
	return rval;
}


/*
 * initialize a dir_jobs array into a free list
 */
 void
dj_freelist_init(struct dir_job *dj_array)
{
	int x;

	dj_freelist = &dj_array[1];
	for (x = 2; x <= nthreads; x++) {
		dj_array[x].forward = &dj_array[x - 1];
		dj_array[x - 1].back = &dj_array[x];
	}
	DBUG("dj_freelist_init: initialized dir_jobs array @ %p with %d elements",
		dj_array, x);
}


/*
 * get an unused dir_job slot from dir_jobs array
 *
 * the plan is to have our own set of allocation routines and allocate
 * dir_jobs from the array that is malloc'd at the start.  this is much, much
 * faster than using malloc and free, and has the nice extra benefit
 * (n_avail_threads variable would no longer be needed) of telling us
 * when we're out of worker threads.
 *
 * queue_lock must be held
 */
 struct dir_job *
dj_calloc(void)
{
	struct dir_job *new_dj;

	new_dj = dj_freelist;
	if (dj_freelist) {
		dj_freelist = dj_freelist->back;
	} else {
		return new_dj;
	}
	if (dj_freelist) {
		dj_freelist->forward = NULL;
	}

	new_dj->forward = new_dj->back = NULL;

	return new_dj;
	//return malloc(sizeof(struct dir_job));
}


/*
 * clear out the dir_job entry in the dir_jobs array.
 * queue_lock must be held by caller.
 */
 void
dj_free(struct dir_job *rel_dj)
{
	TZERO_DJ(rel_dj);
	if (dj_freelist) {
		dj_freelist->forward = rel_dj;
		rel_dj->back = dj_freelist;
	}

	dj_freelist = rel_dj;
	//memset((void *)del_dj, 0, sizeof(struct dir_job));
}


/*
 * queue list data structure and routines.
 * these routines manage the double-linked list queue_list which is
 * used to keep track of the dir_jobs that need doing.
 *
 * queue_lock must be held by caller of all ql_ and dequeue functions
 */
struct queue_list {
	struct queue_list *up;
	struct queue_list *down;
	struct dir_job *djob;
} *qlist, *qlist_tail;


/*
 * grab an unused qlist struct from the array
 *
 * see comments for get_dj_slot() for a glimpse of the future of this function
 */
 struct queue_list *
ql_get_item(void)
{
	return calloc(1, sizeof(struct queue_list));
}


/*
 * release a qlist struct to the free list
 */
 void
ql_rel_item(struct queue_list *ql_item)
{
	//memset(ql_item, 0, sizeof(struct queue_list));
	free(ql_item);
}


/*
 * get a count of items in the queue
 */
 int
size_of_qlist(void)
{
	int qsize;
	struct queue_list *ql_item;

	qsize = 0;
	ql_item = qlist;
	while (ql_item) {
		qsize++;
		ql_item = ql_item->down;
	}

	return qsize;
}


/*
 * remove a dirjob from the queue, and return a pointer to it
 */
 struct queue_list *
ql_get_top(void)
{
	struct queue_list *top_item;

	top_item = qlist;
	if (qlist) {
		qlist = qlist->down;
	} else {
		return top_item;
	}
	if (qlist) {
		qlist->up = NULL;
	} else {
		qlist_tail = qlist;
	}

	return top_item;
}


/*
 * add a new dir job to the tail of the queue
 */
 int
ql_add(struct dir_job *new_dir_job)
{
	struct queue_list *new_ql_item;
	int serrno;

	new_ql_item = ql_get_item();
	if (new_ql_item == NULL) {
		serrno = errno;
		MBUG(" ql_add - malloc of new_ql_item failed");
		return serrno;
	}

	new_ql_item->djob = new_dir_job;
	new_ql_item->up = qlist_tail;
	if (qlist_tail) {
		qlist_tail->down = new_ql_item;
	} else {
		qlist = new_ql_item;
	}
	qlist_tail = new_ql_item;

	return 0;
}


/*
 * wait for the threads associated with a particular dirid
 */
 int
thread_pool_wait(uint64_t did)
{
	int iter;
	int x;
	useconds_t sleep_dur;

	iter = 0;
	sleep_dur = 150000;  /* initial sleep duration 250 msecs */

	if (shutdown_time) {
		join_pool();
		return 0;
	}
	/*
	 * since we're not aquiring the lock covering threads, assume
	 * that a race condition could cause new threads to show up after
	 * not finding any the first time through.  multiple iterations
	 * should cover it, but testing will show the validity of that.
	 *
	 * we don't care about false positives due to race conditions since
	 * we will be iterating again in a moment.
	 */
	while (iter < 2) {
		x = size_of_qlist();
		if ((x > 0) && (!shutdown_time)) {
			//pthread_mutex_lock(&queue_lock);
			pthread_cond_broadcast(&queue_cv);
			//pthread_mutex_unlock(&queue_lock);
		}
		/* a lot can happen after signalling a cv */
		if (shutdown_time) {
			join_pool();
			return 0;
		}
		MBUG(" _pool_wait: queue size = %d", x);
		/* sleep for slightly longer each iteration */
		while (usleep(sleep_dur + (useconds_t)(iter * 20000)) == -1) {
			if (errno == EINTR) {
				/* just continue if sleep interrupted */
				WARN("thread_pool_wait: usleep interrupted");
				continue;
			} else {
				FERR("thread_pool_wait: usleep failed with errno %d",
					errno);
				return -1;
			}
		}
		for (x = 1; x <= nthreads; x++) {
			/* check for threads with the did */
			if (threads[x].job_id == did) {
				MBUG("found thread %02d with job_id %lu", x, did);
				iter = iter - 1;
				break;
			}
		}
		if (x > nthreads) {
			MBUG("no threads found with job_id %lu", did);
		}

		/* 3 times through with none found: assume no more are coming */
		iter = iter + 1;
	}

	return 0;
}


 void
usage(char *prog_name)
{
	char *basename;
	const char *fmt;

	basename = strrchr(prog_name, '/');
	if (basename) {
		basename++;
	} else {
		basename = prog_name;
	}
	fmt = "\nusage:\n%s [-h] [-n N]" 
#ifdef MDEBUG
		" [-d]" 
#endif
		" <path> <user> <group>\n";
	printf(fmt, basename);
	printf("\twhere path is the FQ path of the heirarchy to process, and\n");
	printf("\tuser/group is either the user/group name or the numeric\n");
	printf("user/group id to set as the new owner/group of the files\n");
	printf("\tin the specified path\n");
	printf("\t-h\thelp message\n");
#ifdef MDEBUG
	printf("\t-d\ttoggle debugging output\n");
#endif
	printf("\t-n N\tuse a thread pool with N threads, which must be less\n");
	printf("\t\tthan the calculated number of threads or it will be ignored\n");
}


 void
main(int argc, char **argv)
{
	int ncores;
	uid_t uid;
	gid_t gid;
	uint64_t dirid;
	int i;
	char optret;
	int m;                 /* return value saver */
	int argcnt;
	int user_thr_cnt;
	struct rlimit limits;
	extern char *optarg;
	extern int optind, opterr, optopt;
	char user_name[64];
	char group_name[64];
	struct passwd *pw_entry;
	struct group *gr_entry;

	user_thr_cnt = 0;

#define OPTSTR "hdn:"
	argcnt = argc - 1;
	optret = getopt(argc, argv, OPTSTR);
	while ((optret != -1) && (optret != '?')) {
		switch (optret) {
			case ':':
				printf("\nMissing option argument for option '%c'\n",
					(char)optopt); 
			case 'h':     /* issue help/usage message and exit */
				usage(argv[0]);
				exit(0);
			case 'd':     /* turn on debug messages */
#ifdef MDEBUG
				debug ^= debug;
				argcnt--;
#else
				usage(argv[0]);
				printf("\n-d option not available - not compiled with debug\n");
				exit(0);
#endif
				break;
			case 'n':
				i = sscanf(optarg, "%d", &m);
				if (i != 1) {
					usage(argv[0]);
					printf("\nCould not process '%s' as a thread count\n",
						optarg);
					exit(1);
				}
				if (m > 0) {
					user_thr_cnt = m;
				}
				argcnt = argcnt - 2;
				break;
		}
		optret = getopt(argc, argv, OPTSTR);
	}
	if (optret == '?') {
		usage(argv[0]);
		exit(1);
	}

	/*
	 * count the online logical cores of the CPU(s)
	 */
	ncores = get_core_count();

	/*
	 * nthreads is roughly 90% of the available logical cores
	 */
	nthreads = (int)((float)ncores * .9);
	DBUG("calculated nthreads of %d from %d cores", nthreads, ncores);

	/* this could be cleaner, or clearer */
	if (user_thr_cnt > 0) {
		if (user_thr_cnt < nthreads) {
			nthreads = user_thr_cnt;
		} else {
			WARN("specified thread count of %d is greater than nthreads (%d).  "
				"ignoring...", user_thr_cnt, nthreads);
		}
	}
	DBUG("nthreads set at %d", nthreads);

	/* allocate dir_jobs array */
	dir_jobs = calloc((size_t)(nthreads + 1), sizeof(struct dir_job));
	if (dir_jobs == NULL) {
		FERR("Failed to allocate memory for dir_jobs, errno = %d", errno);
		exit(1);
	}
	DBUG("dir_jobs array allocated @ %p size %d entries %d bytes", dir_jobs,
		nthreads + 1, ((int)sizeof(struct dir_job) * (nthreads + 1)));
	DBUG("sizeof struct dir_job %#lx bytes", sizeof(struct dir_job));
	dj_freelist_init(dir_jobs);

	if (argcnt != 3) {
		usage(argv[0]);
		printf("\n%d - wrong number of arguments\n", argc);
		exit(1);
	}

	/* this should be higher up, and, it needs to be able to use
	 * user names as well as numeric uid, AND, it needs to check them
	 * for validity.  although not the u/gids, I guess
	 */

	/* set the directory head from the invocation argument */
	dir_jobs[0].path = argv[optind++];
	i = sscanf(argv[optind], "%u", &uid);
	if (i != 1) {
		i = sscanf(argv[optind], "%62s", user_name);
		user_name[62] = '\0';
		if (i != 1) {
			usage(argv[0]);
			printf("\nCould not input '%s' as a numeric UID or user name\n",
				argv[optind]);
			exit(1);
		}
		pw_entry = getpwnam((const char *)user_name);
		if (pw_entry == NULL) {
			usage(argv[0]);
			printf("\nCould not process '%s' as a user name, errno '%d'\n",
				argv[optind], errno);
			exit(1);
		}
		uid = pw_entry->pw_uid;
	}
	optind++;
	i = sscanf(argv[optind], "%u", &gid);
	if (i != 1) {
		i = sscanf(argv[optind], "%62s", group_name);
		group_name[62] = '\0';
		if (i != 1) {
			usage(argv[0]);
			printf("\nCould not input '%s' as a numeric GID or group name\n",
				argv[optind]);
			exit(1);
		}
		gr_entry = getgrnam((const char *)group_name);
		if (gr_entry == NULL) {
			usage(argv[0]);
			printf("\nCould not process '%s' as a group name, errno '%d'\n",
				argv[optind], errno);
			exit(1);
		}
		gid = gr_entry->gr_gid;
	}
	optind++;
	DBUG("mchown invoked with path '%s' uid %d gid %d", dir_jobs[0].path, uid,
		gid);

	/*
	 * look at some rlimits and set them if necessary
	 */
	/* open file descriptors */
	if (getrlimit(RLIMIT_NOFILE, &limits) == -1) {
		FERR("getrlimit(OPEN_FILES) returned -1, errno = %d", errno);
	}
	DBUG("invoked open file descriptors: %lu", limits.rlim_max);
	if ((int)limits.rlim_max < (nthreads * 100)) {
		limits.rlim_max = (unsigned long)(nthreads * 100);
		setrlimit(RLIMIT_NOFILE, &limits);
		DBUG("open file descriptors set: %lu", limits.rlim_max);
	}

	/* max stack size */
	if (getrlimit(RLIMIT_STACK, &limits) == -1) {
		FERR("getrlimit(STACKSIZE) returned -1, errno = %d", errno);
	}
	DBUG("invoked max stack size: %ld", (long int)limits.rlim_max);
	DBUG("invoked soft stack size: %ld", (long int)limits.rlim_cur);
	if (limits.rlim_max < (unsigned long)(nthreads * (8192 * 1024))) {
		/* 8MB per thread */
		limits.rlim_max = (unsigned long)(nthreads * (8192 * 1024));
		setrlimit(RLIMIT_STACK, &limits);
		DBUG("max stack size set: %lu", limits.rlim_max);
	}


	/* allocate queue_list array  - not currently used in this version */
	/*
	queue_list = calloc(nthreads, sizeof(struct queue_list));
	if (queue_list == NULL) {
		FERR("Failed to allocate memory for queue_list, errno = %d", errno);
		exit(1);
	}
	DBUG("queue_list array allocated size %lu bytes",
		(sizeof(struct queue_list) * nthreads));
	 */

	/* initialize queue mutext and condition variables */
	pthread_mutex_init(&queue_lock, NULL);
	pthread_cond_init(&queue_cv, NULL);
	DBUG("queue_lock and queue_cv initialized");

	/*
	 * create the pool of threads
	 * the threads struct is created with one slot more than nthreads
	 * in order to store thread_num in slot 0 for the main thread
	 *
	 * create_pool rarely fails, but issues it's own error msg when it does
	 */
	if (create_pool(nthreads)) {
		exit(1);
	}
	DBUG("thread pool successfully created");
	my_tpool = &threads[0];

	/*
	 * allocate cred_table array
	 */
	/* not really used in this phase 1 version, but will be used in daemon */
	cred_tbl = calloc((size_t)(nthreads + 1), sizeof(struct creds));
	if (cred_tbl == NULL) {
		FERR("Failed to allocate memory for cred_tbl, errno = %d", errno);
		exit(1);
	}
	MBUG("cred_tbl array allocated size %d bytes",
		(int)sizeof(struct creds) * (nthreads + 1));

	/*
	 * seed the cred_tbl with -1 because that's how we know that slot is unused
	 */
	for (i = 0; i <= nthreads; i++) {
		cred_tbl[i].u = (uid_t)-1;
		cred_tbl[i].g = (gid_t)-1;
	}

	/*
	 * dir_jobs[0] is used for main thread with invocation path
	 */
	dir_jobs[0].ucred = get_cred(uid, gid);
	if (dir_jobs[0].ucred == NULL) {
		exit(1);
	}
	/* create the dirid for this invocation */
	dir_jobs[0].job_id = mk_dirid(dir_jobs[0].path, dir_jobs[0].ucred);
	MBUG("invocation dirjob created with job_id %lu", dir_jobs[0].job_id);

	/* all this to allocate the dirent structure */
	dname_max = pathconf(dir_jobs[0].path, _PC_NAME_MAX);
	if (dname_max == -1) {         /* not defined or error */
		dname_max = 255;           /* guess */
	}
	MBUG("pathconf returned %d bytes", dname_max);
	dname_max = (int)offsetof(struct dirent, d_name) + dname_max + 1;

	/*
	 * start processing of directories with the invocation dir
	 */
	m = mdpf(&dir_jobs[0]);
	if (m != 0) {
		FERR("main invo mdpf returned %d", m);
		//exit(1);
	}

	/*
	 * try to wait until there are no more threads working on this job_id
	 */
	thread_pool_wait(dir_jobs[0].job_id);

	/*
	 * put the cred slot for this job_id back to unused state
	 */
	rel_cred(dir_jobs[0].ucred);

	printf("files processed: %lu\n", files_chowned + dirs_chowned);
}


/*
 * queue handling functions
 */


/*
 * remove a dir job from the queue, and return a pointer to it
 * queue_lock must be held by caller.
 */
 struct dir_job *
dequeue(void)
{
	struct queue_list *top_item;
	struct dir_job *top_dj;

	top_dj = NULL;
	top_item = ql_get_top();  /* pop the top item off the queue */
	if (top_item) {
		top_dj = top_item->djob;
		MBUG(" dequeue - retrieved djob %p path '%s'", top_dj, top_dj->path);
		ql_rel_item(top_item);
	} else {
		MBUG(" dequeue - queue empty");
	}

	return top_dj;
}


/*
 * add a job to the queue
 * queue_lock must NOT be held by caller
 */
 int
enqueue(char *dpath, char *name, struct creds *creds, uint64_t dj_id)
{
	int add_status;
	struct dir_job *dj_ent;
	char *npath;

	MBUG(" enqueue - called with '%s/%s'", dpath, name);

	if ((strlen(dpath) + strlen(name)) > (DJ_PATH_SZ - 2)) {
		FERR("[%02d] Fail: size of path/name (%lu) in enqueue exceeds "
			"DJ_PATH_SZ (%d)", my_tpool->thread_num,
			strlen(dpath) + strlen(name), DJ_PATH_SZ);
			return 0;
	}

	if (shutdown_time) {
		MBUG(" enqueue returning nak - shutdown is set");
		return 0;
	}

	npath = malloc(DJ_PATH_SZ);
	if (npath == NULL) {
		FERR("[%02d] Failed allocating memory for path in enqueue errno = %d",
			my_tpool->thread_num, errno);
		return 0;
	}
	strncpy(npath, dpath, DJ_PATH_SZ - 1);
	npath[DJ_PATH_SZ - 1] = '\0';
	/* if strlen(npath) >= DJ_PATH_SZ at this point, we're screwed */
	strncat(npath, "/", 2);
	strncat(npath, name, DJ_PATH_SZ - 1);

	pthread_mutex_lock(&queue_lock);

/*
	if (n_avail_threads == 0) {
		pthread_mutex_unlock(&queue_lock);
		free(npath);
		MBUG(" enqueue - no avail threads");
		return 0;
	}
	n_avail_threads--;
	MBUG(" enqueue - n_avail_threads now is %d", n_avail_threads);
 */
	dj_ent = dj_calloc();
	if (dj_ent == NULL) {
		//n_avail_threads++;
		pthread_mutex_unlock(&queue_lock);
		free(npath);
		MBUG(" enqueue - no avail dirjob slots");
		return 0;
	}
	dj_ent->path = npath;
	dj_ent->ucred = creds;
	dj_ent->job_id = dj_id;
	MBUG(" enqueue - queing djob %p path '%s'", dj_ent, dj_ent->path);
	add_status = ql_add(dj_ent);
	pthread_cond_broadcast(&queue_cv);
	pthread_mutex_unlock(&queue_lock);

	return 1;
}

