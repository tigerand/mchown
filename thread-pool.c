/*
 * Copyright 2020-2022 Andrew Sharp andy@tigerand.com, All Rights Reserved
 */

/*
 * thread pool routines
 */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "mchown.h"

int shutdown_time;    /* used to tell the threads not to grab any more dirs */

struct thread_pool *threads;
__thread struct thread_pool *my_tpool;


/*
 * join all the threads. they should be about to die.
 */
 void
join_pool(void)
{
	int t;
	int serrno;
	void *tstatus;
	int jstatus;
	char estrr[128];

	for (t = 1; t <= nthreads; t++) {
		jstatus = pthread_join(threads[t].pthread_id, &tstatus);
		if (jstatus == -1) {
			serrno = errno;
			strerror_r(serrno, estrr, 128);
			FERR("Failure joining thread %02d errno %d - %s", t, serrno, estrr);
		} else if (tstatus == PTHREAD_CANCELED) {
			WARN("thread %02d was previously cancelled", t);
		}
	}
}


/*
 * this is the function that the threads are started with.
 * it waits for dirs to appear on the work queue.
 */
 void *
get_dir_from_queue(void *tpool_entry)
{
	struct dir_job *dir_info;

	my_tpool = (struct thread_pool *)tpool_entry;

	dir_info = NULL;
	//pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	while (! shutdown_time) {
		my_tpool->busy = 0;
		my_tpool->job_id = 0;
		/* acquire the lock and cv, and wait */
		pthread_mutex_lock(&queue_lock);
		//n_avail_threads++;
		if (dir_info) {
			MBUG(" freeing dir_info @ %p", dir_info);
			dj_free(dir_info);       /* free dir_info inside the lock */
		}
		pthread_cond_wait(&queue_cv, &queue_lock);
		dir_info = dequeue();  /* grab a dir_job from the queue */
		if (dir_info) {
			my_tpool->busy = 1;
			my_tpool->job_id = dir_info->job_id;
		//} else { n_avail_threads--;
		}
		pthread_mutex_unlock(&queue_lock);
		if (dir_info) {
			(void)mdpf(dir_info);
			free(dir_info->path);    /* only the enqueue/dequeue code path
                                      * allocates path ... for now */
		}
	}

	pthread_exit(NULL);

	return NULL;
}


/*
 * send a cancel to all the threads
 * @last_cid - call with nthreads to cancel them all, else just the
 *             threads with tid << last_tid
 */
 void
cancel_pool_threads(int last_tid)
{
	int cid;

	for (cid = 1; cid <= last_tid; cid++) {
		pthread_cancel(threads[cid].pthread_id);
	}
}


/*
 * create the pool of threads.  nthreads is the number of threads to
 * create and is calculated in the main line
 */

 int
create_pool(int npthreads)
{
	int tid;
	int status;

	/* allocate 1 extra to store info about the main process thread */
	npthreads = npthreads + 1;
	threads = calloc((size_t)npthreads, sizeof(struct thread_pool));
	if (threads == NULL) {
		status = errno;
		FERR("Failed to allocate threads array.  errno=%d", errno);
		return status;
	}
	DBUG(" struct threads size %d bytes allocated",
		(int)sizeof(struct thread_pool) * npthreads);

	for (tid = 1; tid < npthreads; tid++){
		threads[tid].thread_num = tid;
		/*
		 * call stores the thread_id into the corresponding
		 * element of threads
		 */
		status = pthread_create(&threads[tid].pthread_id,
			(const pthread_attr_t *)NULL, get_dir_from_queue,
			(void *)&threads[tid]);
		if (status != 0) {
			FERR("Failed to create thread id=%d for pool.  Errno=%d", tid,
				status);
			FERR("Shutting down");
			cancel_pool_threads(tid - 1);
			return status;
		}

		DBUG(" thread %02d successfully fjorked", tid);
	}
	sched_yield();  /* grease the pool threads into the cv */

	return 0;
}


