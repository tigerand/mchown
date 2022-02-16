/*
 * Copyright 2020-2022 Andrew Sharp andy@tigerand.com, All Rights Reserved
 */

#if !defined(DAEMON_MODE)
# define FERR(FMT, ...) fprintf(stderr, FMT "\n", ##__VA_ARGS__)
# define WARN(FMT, ...) fprintf(stderr, FMT "\n", ##__VA_ARGS__)
#else
# define FERR(FMT, ...) fprintf(ERRLOG, FMT "\n", ##__VA_ARGS__)
# define WARN(FMT, ...) if (log_level >= LOG_WARN) \
							fprintf(ERRLOG, FMT "\n", ##__VA_ARGS__)
#endif


#ifdef MDEBUG
extern int debug;
# define DBUG(FMT, ...) if (debug) fprintf(stderr, FMT "\n", ##__VA_ARGS__)
# define MBUG(FMT, ...) if (debug) \
	fprintf(stderr, "[%02d] " FMT "\n", my_tpool->thread_num, ##__VA_ARGS__)
#else
# define MBUG(FMT, ...) {}
# define DBUG(FMT, ...) {}
#endif


/*
 * the core data structure definitions for mchown
 */

/*
 * this is the structure that is on the queue, and tells mdpf what
 * directory to proces, as well as a few other important bits
 */
struct dir_job {
	unsigned char *path;
	struct creds *ucred;
	uint64_t job_id;  /* used to tag all the threads working on a particular
					   * heirarchy */
	struct dir_job *forward;
	struct dir_job *back;
};

#define TZERO_DJ(D)	(D)->path =  NULL; \
					(D)->ucred =  NULL; \
					(D)->job_id = 0UL


extern struct dir_job *dj_freelist;

struct thread_pool {
	pthread_t pthread_id;
	int thread_num;
	unsigned int busy;
	uint64_t job_id;
};

struct dir_job *dequeue(void);
int create_pool(int nthreads);
int mdpf(struct dir_job *dj);
void join_pool(void);
void dj_free(struct dir_job *del_dj);

extern struct thread_pool *threads;
extern __thread struct thread_pool *my_tpool;
extern pthread_mutex_t queue_lock;
extern pthread_cond_t queue_cv;
extern int shutdown_time;
extern int nthreads;
//extern int n_avail_threads;
