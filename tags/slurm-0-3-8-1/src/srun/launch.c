/****************************************************************************\
 *  launch.c - initiate the user job's tasks.
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <grondona@llnl.gov>.
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>

#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"

#include "src/srun/job.h"
#include "src/srun/launch.h"
#include "src/srun/opt.h"
#include "src/srun/env.h"

extern char **environ;

/* number of active threads */
static pthread_mutex_t active_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  active_cond  = PTHREAD_COND_INITIALIZER;
static int             active = 0;
static int             fail_launch_cnt = 0;

typedef enum {DSH_NEW, DSH_ACTIVE, DSH_DONE, DSH_FAILED} state_t;

typedef struct task_info {
	slurm_msg_t *req;
	job_t *job;
} task_info_t;

typedef struct thd {
        pthread_t	thread;			/* thread ID */
        state_t		state;      		/* thread state */
	time_t          tstart;			/* time thread started */
	task_info_t     task;
} thd_t;

static int    _check_pending_threads(thd_t *thd, int count);
static void   _spawn_launch_thr(thd_t *th);
static int    _wait_on_active(thd_t *thd, job_t *job);
static void   _p_launch(slurm_msg_t *req_array_ptr, job_t *job);
static void * _p_launch_task(void *args);
static void   _print_launch_msg(launch_tasks_request_msg_t *msg, 
		                char * hostname);

int 
launch_thr_create(job_t *job)
{
	int e;
	pthread_attr_t attr;

	slurm_attr_init(&attr);
	if ((e = pthread_create(&job->lid, &attr, &launch, (void *) job))) 
		slurm_seterrno_ret(e);

	debug("Started launch thread (%lu)", (unsigned long) job->lid);

	return SLURM_SUCCESS;
}

void *
launch(void *arg)
{
	slurm_msg_t *req_array_ptr;
	launch_tasks_request_msg_t *msg_array_ptr;
	job_t *job = (job_t *) arg;
	int i, my_envc;
	char hostname[MAXHOSTNAMELEN];

	update_job_state(job, SRUN_JOB_LAUNCHING);
	if (gethostname(hostname, MAXHOSTNAMELEN) < 0)
		error("gethostname: %m");

	debug("going to launch %d tasks on %d hosts", opt.nprocs, job->nhosts);
	debug("sending to slurmd port %d", slurm_get_slurmd_port());

	msg_array_ptr = 
		xmalloc(sizeof(launch_tasks_request_msg_t)*job->nhosts);
	req_array_ptr = xmalloc(sizeof(slurm_msg_t) * job->nhosts);
	my_envc = envcount(environ);
	for (i = 0; i < job->nhosts; i++) {
		launch_tasks_request_msg_t *r = &msg_array_ptr[i];
		slurm_msg_t                *m = &req_array_ptr[i];

		/* Common message contents */
		r->job_id          = job->jobid;
		r->uid             = opt.uid;
		r->argc            = remote_argc;
		r->argv            = remote_argv;
		r->cred            = job->cred;
		r->job_step_id     = job->stepid;
		r->envc            = my_envc;
		r->env             = environ;
		r->cwd             = opt.cwd;
		r->nnodes          = job->nhosts;
		r->nprocs          = opt.nprocs;
		r->slurmd_debug    = opt.slurmd_debug;
		r->switch_job      = job->switch_job;

		if (job->ofname->type == IO_PER_TASK)
			r->ofname  = job->ofname->name;
		if (job->efname->type == IO_PER_TASK)
			r->efname  = job->efname->name;
		if (job->ifname->type == IO_PER_TASK)
			r->ifname  = job->ifname->name;

		if (opt.parallel_debug)
			r->task_flags |= TASK_PARALLEL_DEBUG;

		/* Node specific message contents */
		if (opt.mpi_type == MPI_LAM)
			r->tasks_to_launch = 1; /* just launch one task */
		else
			r->tasks_to_launch = job->ntask[i];
		r->global_task_ids = job->tids[i];
		r->cpus_allocated  = job->cpus[i];
		r->srun_node_id    = (uint32_t)i;
		r->io_port         = ntohs(job->ioport[i%job->niofds]);
		r->resp_port       = ntohs(job->jaddr[i%job->njfds].sin_port);
		m->msg_type        = REQUEST_LAUNCH_TASKS;
		m->data            = &msg_array_ptr[i];
		memcpy(&m->address, &job->slurmd_addr[i], sizeof(slurm_addr));
	}

	_p_launch(req_array_ptr, job);

	xfree(msg_array_ptr);
	xfree(req_array_ptr);

	if (fail_launch_cnt) {
		job_state_t jstate;

		slurm_mutex_lock(&job->state_mutex);
		jstate = job->state;
		slurm_mutex_unlock(&job->state_mutex);

		if (jstate < SRUN_JOB_TERMINATED) {
			error("%d launch request%s failed", 
			      fail_launch_cnt, fail_launch_cnt > 1 ? "s" : "");
			job->rc = 124;
			job_kill(job);
		}

	} else {
		debug("All task launch requests sent");
		update_job_state(job, SRUN_JOB_STARTING);
	}

	return(void *)(0);
}

static int _check_pending_threads(thd_t *thd, int count)
{
	int i;
	time_t now = time(NULL);

	for (i = 0; i < count; i++) {
		thd_t *tp = &thd[i];
		if ((tp->state == DSH_ACTIVE) && ((now - tp->tstart) >= 10) ) {
			debug2("sending SIGALRM to thread %lu", 
				(unsigned long) tp->thread);
			/*
			 * XXX: sending sigalrm to threads *seems* to
			 *  generate problems with the pthread_manager
			 *  thread. Disable this signal for now
			 * pthread_kill(tp->thread, SIGALRM);
			 */
		}
	}

	return 0;
}

/*
 * When running under parallel debugger, do not create threads in 
 *  detached state, as this seems to confuse TotalView specifically
 */
static void _set_attr_detached (pthread_attr_t *attr)
{
	int err;
	if (!opt.parallel_debug)
		return;
	if ((err = pthread_attr_setdetachstate(attr, PTHREAD_CREATE_DETACHED)))
		error ("pthread_attr_setdetachstate: %s", slurm_strerror(err));
	return;
}

static void _join_attached_threads (int nthreads, thd_t *th)
{
	int i;
	void *retval;
	for (i = 0; i < nthreads; i++)
		pthread_join (th[i].thread, &retval);
	return;
}


static void _spawn_launch_thr(thd_t *th)
{
	pthread_attr_t attr;
	int err = 0;

	slurm_attr_init (&attr);
	_set_attr_detached (&attr);

	err = pthread_create(&th->thread, &attr, _p_launch_task, (void *)th);
	if (err) {
		error ("pthread_create: %s", slurm_strerror(err));

		/* just run it under this thread */
		_p_launch_task((void *) th);
	}

	return;
}

static int _wait_on_active(thd_t *thd, job_t *job)
{
	struct timeval now;
	struct timespec timeout;
	int rc;

	gettimeofday(&now, NULL);
	timeout.tv_sec  = now.tv_sec + 1;
	timeout.tv_nsec = now.tv_usec * 1000;

	rc = pthread_cond_timedwait( &active_cond, 
			             &active_mutex,
			             &timeout      );

	if (rc == ETIMEDOUT)
		_check_pending_threads(thd, job->nhosts);

	return rc;
}

/* _p_launch - parallel (multi-threaded) task launcher */
static void _p_launch(slurm_msg_t *req, job_t *job)
{
	int i;
	thd_t *thd;
	int rc = 0;

	/*
	 * SigFunc *oldh;
	 * sigset_t set;
	 * 
	 * oldh = xsignal(SIGALRM, (SigFunc *) _alrm_handler);
	 * xsignal_save_mask(&set);
	 * xsignal_unblock(SIGALRM);
	 */

	/*
	 * Set job timeout to maximum launch time + current time
	 */
	job->ltimeout = time(NULL) + opt.max_launch_time;

	thd = xmalloc (job->nhosts * sizeof (thd_t));
	for (i = 0; i < job->nhosts; i++) {

		if (job->ntask[i] == 0)	{	/* No tasks for this node */
			debug("Node %s is unused",job->host[i]);
			job->host_state[i] = SRUN_HOST_REPLIED;
			continue;
		}

		pthread_mutex_lock(&active_mutex);
		while (active >= opt.max_threads || rc < 0) 
			rc = _wait_on_active(thd, job);

		active++;
		pthread_mutex_unlock(&active_mutex);

		if (job->state > SRUN_JOB_LAUNCHING)
			break;

		thd[i].task.req = &req[i];
		thd[i].task.job = job;

		_spawn_launch_thr(&thd[i]);
	}

	pthread_mutex_lock(&active_mutex);
	while (active > 0) 
		_wait_on_active(thd, job);
	pthread_mutex_unlock(&active_mutex);

	/*
	 * Need to join with all attached threads if running
	 *  under parallel debugger
	 */
	_join_attached_threads (job->nhosts, thd);

	/*
	 * xsignal_restore_mask(&set);
	 * xsignal(SIGALRM, oldh);
	 */

	xfree(thd);
}

static int
_send_msg_rc(slurm_msg_t *msg)
{
	int rc     = 0;
	int errnum = 0;

       	if ((rc = slurm_send_recv_rc_msg(msg, &errnum, opt.msg_timeout)) < 0) 
		return SLURM_ERROR;

	if (errnum != 0)
		slurm_seterrno_ret (errnum);

	return SLURM_SUCCESS;
}

static void
_update_failed_node(job_t *j, int id)
{
	int i;
	pthread_mutex_lock(&j->task_mutex);
	if (j->host_state[id] == SRUN_HOST_INIT)
		j->host_state[id] = SRUN_HOST_UNREACHABLE;
	for (i = 0; i < j->ntask[id]; i++)
		j->task_state[j->tids[id][i]] = SRUN_TASK_FAILED;
	pthread_mutex_unlock(&j->task_mutex);

	/* update_failed_tasks(j, id); */
}

static void
_update_contacted_node(job_t *j, int id)
{
	pthread_mutex_lock(&j->task_mutex);
	if (j->host_state[id] == SRUN_HOST_INIT)
		j->host_state[id] = SRUN_HOST_CONTACTED;
	pthread_mutex_unlock(&j->task_mutex);
}


/* _p_launch_task - parallelized launch of a specific task */
static void * _p_launch_task(void *arg)
{
	thd_t                      *th     = (thd_t *)arg;
	task_info_t                *tp     = &(th->task);
	slurm_msg_t                *req    = tp->req;
	launch_tasks_request_msg_t *msg    = req->data;
	job_t                      *job    = tp->job;
	int                        nodeid  = msg->srun_node_id;
	int                        failure = 0;
	int                        retry   = 3; /* retry thrice */

	th->state  = DSH_ACTIVE;
	th->tstart = time(NULL);

	if (_verbose)
	        _print_launch_msg(msg, job->host[nodeid]);

    again:
	if (_send_msg_rc(req) < 0) {	/* Has timeout */

		if (errno != EINTR)
			verbose("launch error on %s: %m", job->host[nodeid]);

		if ((errno != ETIMEDOUT) 
		    && (job->state == SRUN_JOB_LAUNCHING)
		    && (errno != ESLURMD_INVALID_JOB_CREDENTIAL) 
		    &&  retry--                                  ) {
			sleep(1);
			goto again;
		}

		if (errno == EINTR)
			verbose("launch on %s canceled", job->host[nodeid]);
		else
			error("launch error on %s: %m", job->host[nodeid]);

		_update_failed_node(job, nodeid);

		th->state = DSH_FAILED;

		failure = 1;

	} else 
		_update_contacted_node(job, nodeid);


	pthread_mutex_lock(&active_mutex);
	th->state = DSH_DONE;
	active--;
	fail_launch_cnt += failure;
	pthread_cond_signal(&active_cond);
	pthread_mutex_unlock(&active_mutex);

	return NULL;
}


static void 
_print_launch_msg(launch_tasks_request_msg_t *msg, char * hostname)
{
	int i;
	char tmp_str[10], task_list[4096];

	if (opt.distribution == SRUN_DIST_BLOCK) {
		sprintf(task_list, "%u-%u", 
		        msg->global_task_ids[0],
			msg->global_task_ids[(msg->tasks_to_launch-1)]);
	} else {
		for (i=0; i<msg->tasks_to_launch; i++) {
			sprintf(tmp_str, ",%u", msg->global_task_ids[i]);
			if (i == 0)
				strcpy(task_list, &tmp_str[1]);
			else if ((strlen(tmp_str) + strlen(task_list)) < 
			         sizeof(task_list))
				strcat(task_list, tmp_str);
			else
				break;
		}
	}

	info("launching %u.%u on host %s, %u tasks: %s", 
	     msg->job_id, msg->job_step_id, hostname, 
	     msg->tasks_to_launch, task_list);

	debug3("uid:%ld cwd:%s %d",
		(long) msg->uid, msg->cwd, msg->srun_node_id);
}
