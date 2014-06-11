/*
 * Written by Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stddef.h>
#include <linux/err.h>
#include "internal.h"
#include "thread.h"
#include "clock.h"
#include "sem.h"
#include <trace/events/cobalt-posix.h>

#define SEM_NAMED    0x80000000

static inline struct cobalt_kqueues *sem_kqueue(struct cobalt_sem *sem)
{
	int pshared = !!(sem->flags & SEM_PSHARED);
	return cobalt_kqueues(pshared);
}

int cobalt_sem_destroy_inner(xnhandle_t handle)
{
	struct cobalt_sem *sem;
	int ret = 0;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
	sem = xnregistry_lookup(handle, NULL);
	if (!cobalt_obj_active(sem, COBALT_SEM_MAGIC, typeof(*sem))) {
		ret = -EINVAL;
		goto unlock_error;
	}
	if (--sem->refs) {
		ret = -EBUSY;
	  unlock_error:
		xnlock_put_irqrestore(&nklock, s);
		return ret;
	}

	cobalt_mark_deleted(sem);
	list_del(&sem->link);
	if (xnsynch_destroy(&sem->synchbase) == XNSYNCH_RESCHED) {
		xnsched_run();
		ret = 1;
	}
	
	xnlock_put_irqrestore(&nklock, s);

	xnheap_free(&xnsys_ppd_get(!!(sem->flags & SEM_PSHARED))->sem_heap,
		sem->datp);
	xnregistry_remove(sem->handle);
	
	xnfree(sem);

	return ret;
}

struct cobalt_sem *
cobalt_sem_init_inner(const char *name, struct cobalt_sem_shadow *sm, 
		      int flags, unsigned int value)
{
	struct cobalt_sem *sem, *osem;
	struct cobalt_kqueues *kq;
	struct xnsys_ppd *sys_ppd;
	struct sem_dat *datp;
	int ret, sflags;
	spl_t s;

	if ((flags & SEM_PULSE) != 0 && value > 0) {
		ret = -EINVAL;
		goto out;
	}

	sem = xnmalloc(sizeof(*sem));
	if (sem == NULL) {
		ret = -ENOSPC;
		goto out;
	}

	ksformat(sem->name, sizeof(sem->name), "%s", name);

	sys_ppd = xnsys_ppd_get(!!(flags & SEM_PSHARED));
	datp = xnheap_alloc(&sys_ppd->sem_heap, sizeof(*datp));
	if (datp == NULL) {
		ret = -EAGAIN;
		goto err_free_sem;
	}

	xnlock_get_irqsave(&nklock, s);

	kq = cobalt_kqueues(!!(flags & SEM_PSHARED));
	if (list_empty(&kq->semq))
		goto do_init;

	if (sm->magic != COBALT_SEM_MAGIC &&
	    sm->magic != COBALT_NAMED_SEM_MAGIC)
		goto do_init;

	/*
	 * Make sure we are not reinitializing a valid semaphore. As a
	 * special exception, we allow reinitializing a shared
	 * anonymous semaphore. Rationale: if the process creating
	 * such semaphore exits, we may assume that other processes
	 * sharing that semaphore won't be able to keep on running.
	 */
	osem = xnregistry_lookup(sm->handle, NULL);
	if (!cobalt_obj_active(osem, COBALT_SEM_MAGIC, typeof(*osem)))
		goto do_init;

	if ((flags & SEM_PSHARED) == 0 || sm->magic != COBALT_SEM_MAGIC) {
		ret = -EBUSY;
		goto err_lock_put;
	}

	xnlock_put_irqrestore(&nklock, s);
	cobalt_sem_destroy_inner(sm->handle);
	xnlock_get_irqsave(&nklock, s);
  do_init:
	if (value > (unsigned)SEM_VALUE_MAX) {
		ret = -EINVAL;
		goto err_lock_put;
	}
	
	ret = xnregistry_enter(sem->name, sem, &sem->handle, NULL);
	if (ret < 0)
		goto err_lock_put;

	sem->magic = COBALT_SEM_MAGIC;
	list_add_tail(&sem->link, &kq->semq);
	sflags = flags & SEM_FIFO ? 0 : XNSYNCH_PRIO;
	xnsynch_init(&sem->synchbase, sflags, NULL);

	sem->datp = datp;
	atomic_long_set(&datp->value, value);
	datp->flags = flags;
	sem->flags = flags;
	sem->owningq = kq;
	sem->refs = name[0] ? 2 : 1;
			
	sm->magic = name[0] ? COBALT_NAMED_SEM_MAGIC : COBALT_SEM_MAGIC;
	sm->handle = sem->handle;
	sm->datp_offset = xnheap_mapped_offset(&sys_ppd->sem_heap, datp);
	if (flags & SEM_PSHARED)
		sm->datp_offset = -sm->datp_offset;
	xnlock_put_irqrestore(&nklock, s);

	trace_cobalt_psem_init(sem->name, sem->handle, flags, value);

	return sem;

err_lock_put:
	xnlock_put_irqrestore(&nklock, s);
	xnheap_free(&sys_ppd->sem_heap, datp);
err_free_sem:
	xnfree(sem);
out:
	trace_cobalt_psem_init_failed(name, flags, value, ret);

	return ERR_PTR(ret);
}

static int sem_destroy(struct cobalt_sem_shadow *sm)
{
	struct cobalt_sem *sem;
	int warn, ret;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
	if (sm->magic != COBALT_SEM_MAGIC) {
		ret = -EINVAL;
		goto error;
	}

	sem = xnregistry_lookup(sm->handle, NULL);
	if (!cobalt_obj_active(sem, COBALT_SEM_MAGIC, typeof(*sem))) {
		ret = -EINVAL;
		goto error;
	}

	if (sem_kqueue(sem) != sem->owningq) {
		ret = -EPERM;
		goto error;
	}

	if ((sem->flags & SEM_NOBUSYDEL) != 0 &&
	    xnsynch_pended_p(&sem->synchbase)) {
		ret = -EBUSY;
		goto error;
	}

	warn = sem->flags & SEM_WARNDEL;
	cobalt_mark_deleted(sm);
	xnlock_put_irqrestore(&nklock, s);

	ret = cobalt_sem_destroy_inner(sem->handle);

	return warn ? ret : 0;

      error:

	xnlock_put_irqrestore(&nklock, s);

	return ret;
}

static inline int sem_trywait_inner(struct cobalt_sem *sem)
{
	if (sem == NULL || sem->magic != COBALT_SEM_MAGIC)
		return -EINVAL;

#if XENO_DEBUG(COBALT)
	if (sem->owningq != sem_kqueue(sem))
		return -EPERM;
#endif /* XENO_DEBUG(COBALT) */

	if (atomic_long_sub_return(1, &sem->datp->value) < 0)
		return -EAGAIN;

	return 0;
}

static int sem_trywait(xnhandle_t handle)
{
	int err;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
	err = sem_trywait_inner(xnregistry_lookup(handle, NULL));
	xnlock_put_irqrestore(&nklock, s);

	return err;
}

static inline int
sem_wait_inner(xnhandle_t handle, int timed,
	       const struct timespec __user *u_ts)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
	int pull_ts = 1, ret, info;
	struct cobalt_sem *sem;
	xntmode_t tmode;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
redo:
	sem = xnregistry_lookup(handle, NULL);

	ret = sem_trywait_inner(sem);
	if (ret != -EAGAIN)
		goto out;

	if (timed) {
		/*
		 * POSIX states that the validity of the timeout spec
		 * _need_ not be checked if the semaphore can be
		 * locked immediately, we show this behavior despite
		 * it's actually more complex, to keep some
		 * applications ported to Linux happy.
		 */
		if (pull_ts) {
			atomic_long_inc(&sem->datp->value);
			if (u_ts == NULL)
				goto efault;
			xnlock_put_irqrestore(&nklock, s);
			ret =__xn_safe_copy_from_user(&ts, u_ts, sizeof(ts));
			xnlock_get_irqsave(&nklock, s);
			if (ret)
				goto efault;
			if (ts.tv_nsec >= ONE_BILLION)
				goto einval;
			pull_ts = 0;
			goto redo;
		}

		tmode = sem->flags & SEM_RAWCLOCK ? XN_ABSOLUTE : XN_REALTIME;
		info = xnsynch_sleep_on(&sem->synchbase, ts2ns(&ts) + 1, tmode);
	} else
		info = xnsynch_sleep_on(&sem->synchbase, 
					XN_INFINITE, XN_RELATIVE);
	if (info & XNRMID)
		goto einval;
 	if (info & (XNBREAK|XNTIMEO)) {
		ret = (info & XNBREAK) ? -EINTR : -ETIMEDOUT;
		goto fail;
	}
	ret = 0;
out:
	xnlock_put_irqrestore(&nklock, s);

	return ret;
fail:
	atomic_long_inc(&sem->datp->value);
	goto out;
efault:
	ret = -EFAULT;
	goto out;
einval:
	ret = -EINVAL;
	goto out;
}

static int sem_wait(xnhandle_t handle)
{
	return sem_wait_inner(handle, 0, NULL);
}

static int sem_timedwait(xnhandle_t handle,
			 const struct timespec __user *abs_timeout)
{
	return sem_wait_inner(handle, 1, abs_timeout);
}

int sem_post_inner(struct cobalt_sem *sem, struct cobalt_kqueues *ownq, int bcast)
{
	if (sem == NULL || sem->magic != COBALT_SEM_MAGIC)
		return -EINVAL;

#if XENO_DEBUG(COBALT)
	if (ownq && ownq != sem_kqueue(sem))
		return -EPERM;
#endif /* XENO_DEBUG(COBALT) */

	if (atomic_long_read(&sem->datp->value) == SEM_VALUE_MAX)
		return -EINVAL;

	if (!bcast) {
		if (atomic_long_inc_return(&sem->datp->value) <= 0) {
			if (xnsynch_wakeup_one_sleeper(&sem->synchbase))
				xnsched_run();
		} else if (sem->flags & SEM_PULSE)
			atomic_long_set(&sem->datp->value, 0);
	} else {
		if (atomic_long_read(&sem->datp->value) < 0) {
			atomic_long_set(&sem->datp->value, 0);
			if (xnsynch_flush(&sem->synchbase, 0) == 
				XNSYNCH_RESCHED)
				xnsched_run();
		}
	}

	return 0;
}

static int sem_post(xnhandle_t handle)
{
	struct cobalt_sem *sm;
	int ret;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
	sm = xnregistry_lookup(handle, NULL);
	ret = sem_post_inner(sm, sm->owningq, 0);
	xnlock_put_irqrestore(&nklock, s);

	return ret;
}

static int sem_getvalue(xnhandle_t handle, int *value)
{
	struct cobalt_sem *sem;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	sem = xnregistry_lookup(handle, NULL);

	if (sem == NULL || sem->magic != COBALT_SEM_MAGIC) {
		xnlock_put_irqrestore(&nklock, s);
		return -EINVAL;
	}

	if (sem->owningq != sem_kqueue(sem)) {
		xnlock_put_irqrestore(&nklock, s);
		return -EPERM;
	}

	*value = atomic_long_read(&sem->datp->value);
	if ((sem->flags & SEM_REPORT) == 0 && *value < 0)
		*value = 0;

	xnlock_put_irqrestore(&nklock, s);

	return 0;
}

int cobalt_sem_init(struct cobalt_sem_shadow __user *u_sem, int pshared, unsigned value)
{
	struct cobalt_sem_shadow sm;
	struct cobalt_sem *sem;

	if (__xn_safe_copy_from_user(&sm, u_sem, sizeof(sm)))
		return -EFAULT;

	sem = cobalt_sem_init_inner("", &sm, pshared ? SEM_PSHARED : 0, value);
	if (IS_ERR(sem))
		return PTR_ERR(sem);

	return __xn_safe_copy_to_user(u_sem, &sm, sizeof(*u_sem));
}

int cobalt_sem_post(struct cobalt_sem_shadow __user *u_sem)
{
	xnhandle_t handle;

	handle = cobalt_get_handle_from_user(&u_sem->handle);
	trace_cobalt_psem_post(handle);

	return sem_post(handle);
}

int cobalt_sem_wait(struct cobalt_sem_shadow __user *u_sem)
{
	xnhandle_t handle;

	handle = cobalt_get_handle_from_user(&u_sem->handle);
	trace_cobalt_psem_wait(handle);

	return sem_wait(handle);
}

int cobalt_sem_timedwait(struct cobalt_sem_shadow __user *u_sem,
			 struct timespec __user *u_ts)
{
	xnhandle_t handle;

	handle = cobalt_get_handle_from_user(&u_sem->handle);
	trace_cobalt_psem_timedwait(handle);

	return sem_timedwait(handle, u_ts);
}

int cobalt_sem_trywait(struct cobalt_sem_shadow __user *u_sem)
{
	xnhandle_t handle;

	handle = cobalt_get_handle_from_user(&u_sem->handle);
	trace_cobalt_psem_trywait(handle);

	return sem_trywait(handle);
}

int cobalt_sem_getvalue(struct cobalt_sem_shadow __user *u_sem, int __user *u_sval)
{
	int ret, sval = -1;
	xnhandle_t handle;

	handle = cobalt_get_handle_from_user(&u_sem->handle);

	ret = sem_getvalue(handle, &sval);
	trace_cobalt_psem_getvalue(handle, sval);
	if (ret)
		return ret;

	return __xn_safe_copy_to_user(u_sval, &sval, sizeof(sval));
}

int cobalt_sem_destroy(struct cobalt_sem_shadow __user *u_sem)
{
	struct cobalt_sem_shadow sm;
	int err;

	if (__xn_safe_copy_from_user(&sm, u_sem, sizeof(sm)))
		return -EFAULT;

	trace_cobalt_psem_destroy(sm.handle);

	err = sem_destroy(&sm);
	if (err < 0)
		return err;

	return __xn_safe_copy_to_user(u_sem, &sm, sizeof(*u_sem)) ?: err;
}

int cobalt_sem_init_np(struct cobalt_sem_shadow __user *u_sem,
		       int flags, unsigned value)
{
	struct cobalt_sem_shadow sm;
	struct cobalt_sem *sem;

	if (__xn_safe_copy_from_user(&sm, u_sem, sizeof(sm)))
		return -EFAULT;

	if (flags & ~(SEM_FIFO|SEM_PULSE|SEM_PSHARED|SEM_REPORT|\
		      SEM_WARNDEL|SEM_RAWCLOCK|SEM_NOBUSYDEL))
		return -EINVAL;

	sem = cobalt_sem_init_inner("", &sm, flags, value);
	if (IS_ERR(sem))
		return PTR_ERR(sem);

	return __xn_safe_copy_to_user(u_sem, &sm, sizeof(*u_sem));
}

int cobalt_sem_broadcast_np(struct cobalt_sem_shadow __user *u_sem)
{
	struct cobalt_sem *sm;
	xnhandle_t handle;
	spl_t s;
	int err;

	handle = cobalt_get_handle_from_user(&u_sem->handle);
	trace_cobalt_psem_broadcast(u_sem->handle);

	xnlock_get_irqsave(&nklock, s);
	sm = xnregistry_lookup(handle, NULL);
	err = sem_post_inner(sm, sm->owningq, 1);
	xnlock_put_irqrestore(&nklock, s);

	return err;
}

int cobalt_sem_inquire(struct cobalt_sem_shadow __user *u_sem,
		       struct cobalt_sem_info __user *u_info,
		       pid_t __user *u_waitlist,
		       size_t waitsz)
{
	int val = 0, nrwait = 0, nrpids, ret = 0;
	unsigned long pstamp, nstamp = 0;
	struct cobalt_sem_info info;
	pid_t *t = NULL, fbuf[16];
	struct xnthread *thread;
	struct cobalt_sem *sem;
	xnhandle_t handle;
	spl_t s;

	handle = cobalt_get_handle_from_user(&u_sem->handle);
	trace_cobalt_psem_inquire(handle);

	nrpids = waitsz / sizeof(pid_t);

	xnlock_get_irqsave(&nklock, s);

	for (;;) {
		pstamp = nstamp;
		sem = xnregistry_lookup(handle, &nstamp);
		if (sem == NULL || sem->magic != COBALT_SEM_MAGIC) {
			xnlock_put_irqrestore(&nklock, s);
			return -EINVAL;
		}
		/*
		 * Allocate memory to return the wait list without
		 * holding any lock, then revalidate the handle.
		 */
		if (t == NULL) {
			val = atomic_long_read(&sem->datp->value);
			if (val >= 0 || u_waitlist == NULL)
				break;
			xnlock_put_irqrestore(&nklock, s);
			if (nrpids > -val)
				nrpids = -val;
			if (-val <= ARRAY_SIZE(fbuf))
				t = fbuf; /* Use fast buffer. */
			else {
				t = xnmalloc(-val * sizeof(pid_t));
				if (t == NULL)
					return -ENOMEM;
			}
			xnlock_get_irqsave(&nklock, s);
		} else if (pstamp == nstamp)
			break;
		else if (val != atomic_long_read(&sem->datp->value)) {
			xnlock_put_irqrestore(&nklock, s);
			if (t != fbuf)
				xnfree(t);
			t = NULL;
			xnlock_get_irqsave(&nklock, s);
		}
	}

	info.flags = sem->flags;
	info.value = (sem->flags & SEM_REPORT) || val >= 0 ? val : 0;
	info.nrwait = val < 0 ? -val : 0;

	if (xnsynch_pended_p(&sem->synchbase) && u_waitlist != NULL) {
		xnsynch_for_each_sleeper(thread, &sem->synchbase) {
			if (nrwait >= nrpids)
				break;
			t[nrwait++] = xnthread_host_pid(thread);
		}
	}

	xnlock_put_irqrestore(&nklock, s);

	ret = __xn_safe_copy_to_user(u_info, &info, sizeof(info));
	if (ret == 0 && nrwait > 0)
		ret = __xn_safe_copy_to_user(u_waitlist, t, nrwait * sizeof(pid_t));

	if (t && t != fbuf)
		xnfree(t);

	return ret ?: nrwait;
}

void cobalt_semq_cleanup(struct cobalt_kqueues *q)
{
	struct cobalt_sem *sem, *tmp;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (list_empty(&q->semq))
		goto out;

	list_for_each_entry_safe(sem, tmp, &q->semq, link) {
		xnlock_put_irqrestore(&nklock, s);
		if (sem->flags & SEM_NAMED)
			cobalt_sem_unlink_inner(sem->handle);
		cobalt_sem_destroy_inner(sem->handle);
		xnlock_get_irqsave(&nklock, s);
	}
out:
	xnlock_put_irqrestore(&nklock, s);
}


void cobalt_sem_pkg_init(void)
{
	INIT_LIST_HEAD(&cobalt_global_kqueues.semq);
}

void cobalt_sem_pkg_cleanup(void)
{
	cobalt_semq_cleanup(&cobalt_global_kqueues);
}
