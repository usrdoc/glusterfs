/*
  Copyright (c) 2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include <pthread.h>
#include <stdlib.h>
#include <errno.h>

#include "glusterfs/gf-event.h"
#include "glusterfs/common-utils.h"
#include "glusterfs/syscall.h"
#include "glusterfs/libglusterfs-messages.h"

#ifdef HAVE_SYS_EPOLL_H
#include <sys/epoll.h>

struct event_slot_epoll {
    int slots_used;
    int fd;
    int events;
    int gen;
    int idx;
    gf_atomic_t ref;
    int do_close;
    int in_handler;
    int handled_error;
    void *data;
    event_handler_t handler;
    struct list_head poller_death;
    gf_lock_t lock;
};

struct event_thread_data {
    struct event_pool *event_pool;
    int event_index;
};

static struct event_slot_epoll *
__event_newtable(struct event_pool *event_pool, int table_idx)
{
    struct event_slot_epoll *table = NULL;
    int i;

    table = GF_CALLOC(sizeof(*table), EVENT_EPOLL_SLOTS, gf_common_mt_ereg);
    if (!table)
        return NULL;

    for (i = 0; i < EVENT_EPOLL_SLOTS; i++)
        table[i].fd = -1;

    event_pool->ereg[table_idx] = table;

    return table;
}

static void
event_slot_ref(struct event_slot_epoll *slot)
{
    if (slot)
        GF_ATOMIC_INC(slot->ref);
}

static int
__event_slot_alloc(struct event_pool *event_pool, int fd,
                   int notify_poller_death, struct event_slot_epoll **slot)
{
    int i = 0;
    int j = 0;
    int table_idx;
    int gen;
    struct event_slot_epoll *table = NULL;

retry:

    while (i < EVENT_EPOLL_TABLES) {
        table = event_pool->ereg[i];
        if (table) {
            if (table->slots_used == EVENT_EPOLL_SLOTS)
                return -1;
            else
                break; /* break out of the loop */
        } else {
            table = __event_newtable(event_pool, i);
            if (!table)
                return -1;
            else
                break; /* break out of the loop */
        }

        i++;
    }

    table_idx = i;

    for (j = 0; j < EVENT_EPOLL_SLOTS; j++) {
        if (table[j].fd == -1) {
            /* wipe everything except bump the generation */
            gen = table[j].gen;
            memset(&table[j], 0, sizeof(table[j]));
            table[j].fd = fd;
            table[j].gen = gen + 1;

            INIT_LIST_HEAD(&table[j].poller_death);
            LOCK_INIT(&table[j].lock);

            if (notify_poller_death) {
                table[j].idx = table_idx * EVENT_EPOLL_SLOTS + j;
                list_add_tail(&table[j].poller_death,
                              &event_pool->poller_death);
            }

            event_pool->ereg[table_idx]->slots_used++;

            break;
        }
    }

    if (j == EVENT_EPOLL_SLOTS) {
        table = NULL;
        i++;
        goto retry;
    } else {
        (*slot) = &table[j];
        event_slot_ref(*slot);
        return table_idx * EVENT_EPOLL_SLOTS + j;
    }
}

static void
__event_slot_dealloc(struct event_slot_epoll *table, int offset)
{
    struct event_slot_epoll *slot = NULL;
    int fd;

    slot = &table[offset];
    slot->gen++;

    fd = slot->fd;
    slot->fd = -1;
    slot->handled_error = 0;
    slot->in_handler = 0;
    LOCK_DESTROY(&slot->lock);
    list_del_init(&slot->poller_death);
    if (fd != -1)
        table->slots_used--;
}

static void
event_slot_dealloc(struct event_pool *event_pool, int idx)
{
    int table_idx = idx / EVENT_EPOLL_SLOTS;
    int offset;
    struct event_slot_epoll *table = NULL;

    table = event_pool->ereg[table_idx];
    if (!table)
        return;

    offset = idx % EVENT_EPOLL_SLOTS;
    pthread_mutex_lock(&event_pool->mutex);
    {
        __event_slot_dealloc(table, offset);
    }
    pthread_mutex_unlock(&event_pool->mutex);

    return;
}

static struct event_slot_epoll *
event_slot_get(struct event_pool *event_pool, int idx)
{
    struct event_slot_epoll *slot = NULL;
    struct event_slot_epoll *table = NULL;
    int table_idx = 0;
    int offset = 0;

    table_idx = idx / EVENT_EPOLL_SLOTS;
    offset = idx % EVENT_EPOLL_SLOTS;

    table = event_pool->ereg[table_idx];
    if (!table)
        goto out;

    slot = &table[offset];

    event_slot_ref(slot);

out:
    return slot;
}

static void
__event_slot_unref(struct event_pool *event_pool, struct event_slot_epoll *slot,
                   int idx)
{
    int64_t ref;
    int fd;
    int do_close = 0;
    int table_idx, offset;
    struct event_slot_epoll *table = NULL;

    ref = GF_ATOMIC_DEC(slot->ref);
    if (ref)
        /* slot still alive */
        goto done;

    LOCK(&slot->lock);
    {
        fd = slot->fd;
        do_close = slot->do_close;
        slot->do_close = 0;
    }
    UNLOCK(&slot->lock);

    table_idx = idx / EVENT_EPOLL_SLOTS;

    table = event_pool->ereg[table_idx];
    if (table) {
        offset = idx % EVENT_EPOLL_SLOTS;
        __event_slot_dealloc(table, offset);
    }
    if (do_close)
        sys_close(fd);
done:
    return;
}

static void
event_slot_unref(struct event_pool *event_pool, struct event_slot_epoll *slot,
                 int idx)
{
    int64_t ref;
    int fd;
    int do_close = 0;

    ref = GF_ATOMIC_DEC(slot->ref);
    if (ref)
        /* slot still alive */
        goto done;

    LOCK(&slot->lock);
    {
        fd = slot->fd;
        do_close = slot->do_close;
        slot->do_close = 0;
    }
    UNLOCK(&slot->lock);

    event_slot_dealloc(event_pool, idx);

    if (do_close)
        sys_close(fd);
done:
    return;
}

static struct event_pool *
event_pool_new_epoll(int count, int eventthreadcount)
{
    struct event_pool *event_pool = NULL;
    int epfd;

    event_pool = GF_CALLOC(1, sizeof(*event_pool), gf_common_mt_event_pool);

    if (!event_pool)
        goto out;

    epfd = epoll_create(count);

    if (epfd < 0) {
        goto err;
    }

    if (__event_newtable(event_pool, 0) == NULL) {
        goto err;
    }

    event_pool->fd = epfd;

    event_pool->count = count;
    INIT_LIST_HEAD(&event_pool->poller_death);
    event_pool->eventthreadcount = eventthreadcount;
    event_pool->auto_thread_count = 0;
    pthread_mutex_init(&event_pool->mutex, NULL);

out:
    return event_pool;

err:
    gf_smsg("epoll", GF_LOG_ERROR, errno, LG_MSG_EPOLL_FD_CREATE_FAILED, NULL);
    GF_FREE(event_pool->reg);
    GF_FREE(event_pool);
    return NULL;
}

static void
__slot_update_events(struct event_slot_epoll *slot, int poll_in, int poll_out)
{
    switch (poll_in) {
        case 1:
            slot->events |= EPOLLIN;
            break;
        case 0:
            slot->events &= ~EPOLLIN;
            break;
        case -1:
            /* do nothing */
            break;
        default:
            gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_INVALID_POLL_IN,
                    "value=%d", poll_in, NULL);
            break;
    }

    switch (poll_out) {
        case 1:
            slot->events |= EPOLLOUT;
            break;
        case 0:
            slot->events &= ~EPOLLOUT;
            break;
        case -1:
            /* do nothing */
            break;
        default:
            gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_INVALID_POLL_OUT,
                    "value=%d", poll_out, NULL);
            break;
    }
}

int
event_register_epoll(struct event_pool *event_pool, int fd,
                     event_handler_t handler, void *data, int poll_in,
                     int poll_out, int notify_poller_death)
{
    int idx = -1;
    int ret = -1;
    int destroy = 0;
    struct epoll_event epoll_event = {
        0,
    };
    struct event_data *ev_data = (void *)&epoll_event.data;
    struct event_slot_epoll *slot = NULL;

    GF_VALIDATE_OR_GOTO("event", event_pool, out);

    /* TODO: Even with the below check, there is a possibility of race,
     * What if the destroy mode is set after the check is done.
     * Not sure of the best way to prevent this race, ref counting
     * is one possibility.
     * There is no harm in registering and unregistering the fd
     * even after destroy mode is set, just that such fds will remain
     * open until unregister is called, also the events on that fd will be
     * notified, until one of the poller thread is alive.
     */
    pthread_mutex_lock(&event_pool->mutex);
    {
        destroy = event_pool->destroy;
        if (destroy == 1) {
            pthread_mutex_unlock(&event_pool->mutex);
            goto out;
        }

        idx = __event_slot_alloc(event_pool, fd, notify_poller_death, &slot);
    }
    pthread_mutex_unlock(&event_pool->mutex);

    if (idx < 0) {
        gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_SLOT_NOT_FOUND, "fd=%d", fd,
                NULL);
        return -1;
    }

    assert(slot->fd == fd);

    LOCK(&slot->lock);
    {
        /* make epoll 'singleshot', which
           means we need to re-add the fd with
           epoll_ctl(EPOLL_CTL_MOD) after delivery of every
           single event. This assures us that while a poller
           thread has picked up and is processing an event,
           another poller will not try to pick this at the same
           time as well.
        */

        slot->events = EPOLLPRI | EPOLLHUP | EPOLLERR | EPOLLONESHOT;
        slot->handler = handler;
        slot->data = data;

        __slot_update_events(slot, poll_in, poll_out);

        epoll_event.events = slot->events;
        ev_data->idx = idx;
        ev_data->gen = slot->gen;

        ret = epoll_ctl(event_pool->fd, EPOLL_CTL_ADD, fd, &epoll_event);
        /* check ret after UNLOCK() to avoid deadlock in
           event_slot_unref()
        */
    }
    UNLOCK(&slot->lock);

    if (ret == -1) {
        gf_smsg("epoll", GF_LOG_ERROR, errno, LG_MSG_EPOLL_FD_ADD_FAILED,
                "fd=%d", fd, "epoll_fd=%d", event_pool->fd, NULL);
        event_slot_unref(event_pool, slot, idx);
        idx = -1;
    }

    /* keep slot->ref (do not event_slot_unref) if successful */
out:
    return idx;
}

static int
event_unregister_epoll_common(struct event_pool *event_pool, int fd, int idx,
                              int do_close)
{
    int ret = -1;
    struct event_slot_epoll *slot = NULL;

    GF_VALIDATE_OR_GOTO("event", event_pool, out);

    /* During shutdown, it may happen that a socket registration with
     * the event sub-system may fail and an rpc_transport_unref() may
     * be called for such an unregistered socket with idx == -1. This
     * may cause the following assert(slot->fd == fd) to fail.
     */
    if (idx < 0)
        goto out;

    slot = event_slot_get(event_pool, idx);
    if (!slot) {
        gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_SLOT_NOT_FOUND, "fd=%d", fd,
                "idx=%d", idx, NULL);
        return -1;
    }

    assert(slot->fd == fd);

    LOCK(&slot->lock);
    {
        ret = epoll_ctl(event_pool->fd, EPOLL_CTL_DEL, fd, NULL);

        if (ret == -1) {
            gf_smsg("epoll", GF_LOG_ERROR, errno, LG_MSG_EPOLL_FD_DEL_FAILED,
                    "fd=%d", fd, "epoll_fd=%d", event_pool->fd, NULL);
            goto unlock;
        }

        slot->do_close = do_close;
        slot->gen++; /* detect unregister in dispatch_handler() */
    }
unlock:
    UNLOCK(&slot->lock);

    event_slot_unref(event_pool, slot, idx); /* one for event_register() */
    event_slot_unref(event_pool, slot, idx); /* one for event_slot_get() */
out:
    return ret;
}

static int
event_unregister_epoll(struct event_pool *event_pool, int fd, int idx_hint)
{
    return event_unregister_epoll_common(event_pool, fd, idx_hint, 0);
}

static int
event_unregister_close_epoll(struct event_pool *event_pool, int fd,
                             int idx_hint)
{
    return event_unregister_epoll_common(event_pool, fd, idx_hint, 1);
}

static int
event_select_on_epoll(struct event_pool *event_pool, int fd, int idx,
                      int poll_in, int poll_out)
{
    int ret = -1;
    struct event_slot_epoll *slot = NULL;
    struct epoll_event epoll_event = {
        0,
    };
    struct event_data *ev_data = (void *)&epoll_event.data;

    GF_VALIDATE_OR_GOTO("event", event_pool, out);

    slot = event_slot_get(event_pool, idx);
    if (!slot) {
        gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_SLOT_NOT_FOUND, "fd=%d", fd,
                "idx=%d", idx, NULL);
        return -1;
    }

    assert(slot->fd == fd);

    LOCK(&slot->lock);
    {
        __slot_update_events(slot, poll_in, poll_out);

        epoll_event.events = slot->events;
        ev_data->idx = idx;
        ev_data->gen = slot->gen;

        if (slot->in_handler)
            /*
             * in_handler indicates at least one thread
             * executing event_dispatch_epoll_handler()
             * which will perform epoll_ctl(EPOLL_CTL_MOD)
             * anyways (because of EPOLLET)
             *
             * This not only saves a system call, but also
             * avoids possibility of another epoll thread
             * picking up the next event while the ongoing
             * handler is still in progress (and resulting
             * in unnecessary contention on rpc_transport_t->mutex).
             */
            goto unlock;

        ret = epoll_ctl(event_pool->fd, EPOLL_CTL_MOD, fd, &epoll_event);
        if (ret == -1) {
            gf_smsg("epoll", GF_LOG_ERROR, errno, LG_MSG_EPOLL_FD_MODIFY_FAILED,
                    "fd=%d", fd, "events=%d", epoll_event.events, NULL);
        }
    }
unlock:
    UNLOCK(&slot->lock);

    event_slot_unref(event_pool, slot, idx);

out:
    return idx;
}

static int
event_dispatch_epoll_handler(struct event_pool *event_pool,
                             struct epoll_event *event)
{
    struct event_data *ev_data = NULL;
    struct event_slot_epoll *slot = NULL;
    event_handler_t handler = NULL;
    void *data = NULL;
    int idx;
    int gen;
    int ret = -1;
    int fd = -1;
    gf_boolean_t handled_error_previously = _gf_false;

    ev_data = (void *)&event->data;
    handler = NULL;
    data = NULL;

    idx = ev_data->idx;
    gen = ev_data->gen;

    slot = event_slot_get(event_pool, idx);
    if (!slot) {
        gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_SLOT_NOT_FOUND, "idx=%d", idx,
                NULL);
        return -1;
    }

    LOCK(&slot->lock);
    {
        fd = slot->fd;
        if (fd == -1) {
            gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_STALE_FD_FOUND, "idx=%d",
                    idx, "gen=%d", gen, "events=%d", event->events,
                    "slot->gen=%d", slot->gen, NULL);
            /* fd got unregistered in another thread */
            goto pre_unlock;
        }

        if (gen != slot->gen) {
            gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_GENERATION_MISMATCH,
                    "idx=%d", idx, "gen=%d", gen, "slot->gen=%d", slot->gen,
                    "slot->fd=%d", slot->fd, NULL);
            /* slot was re-used and therefore is another fd! */
            goto pre_unlock;
        }

        handler = slot->handler;
        data = slot->data;

        if (slot->in_handler > 0) {
            /* Another handler is inprogress, skip this one. */
            handler = NULL;
            goto pre_unlock;
        }

        if (slot->handled_error) {
            handled_error_previously = _gf_true;
        } else {
            slot->handled_error = (event->events & (EPOLLERR | EPOLLHUP));
            slot->in_handler++;
        }
    }
pre_unlock:
    UNLOCK(&slot->lock);

    ret = 0;

    if (!handler)
        goto out;

    if (!handled_error_previously) {
        handler(fd, idx, gen, data, (event->events & (EPOLLIN | EPOLLPRI)),
                (event->events & (EPOLLOUT)),
                (event->events & (EPOLLERR | EPOLLHUP)), 0);
    }
out:
    event_slot_unref(event_pool, slot, idx);

    return ret;
}

static void *
event_dispatch_epoll_worker(void *data)
{
    struct epoll_event event;
    int ret = -1;
    struct event_thread_data *ev_data = data;
    struct event_pool *event_pool;
    int myindex;
    int timetodie = 0, gen = 0;
    struct list_head poller_death_notify;
    struct event_slot_epoll *slot = NULL, *tmp = NULL;

    GF_VALIDATE_OR_GOTO("event", ev_data, out);

    event_pool = ev_data->event_pool;
    myindex = ev_data->event_index;

    GF_VALIDATE_OR_GOTO("event", event_pool, out);

    gf_smsg("epoll", GF_LOG_INFO, 0, LG_MSG_STARTED_EPOLL_THREAD, "index=%d",
            myindex - 1, NULL);

    pthread_mutex_lock(&event_pool->mutex);
    {
        event_pool->activethreadcount++;
    }
    pthread_mutex_unlock(&event_pool->mutex);

    for (;;) {
        if (event_pool->eventthreadcount < myindex) {
            /* ...time to die, thread count was decreased below
             * this threads index */
            /* Start with extra safety at this point, reducing
             * lock conention in normal case when threads are not
             * reconfigured always */
            pthread_mutex_lock(&event_pool->mutex);
            {
                if (event_pool->eventthreadcount < myindex) {
                    while (event_pool->poller_death_sliced) {
                        pthread_cond_wait(&event_pool->cond,
                                          &event_pool->mutex);
                    }

                    INIT_LIST_HEAD(&poller_death_notify);
                    /* if found true in critical section,
                     * die */
                    event_pool->pollers[myindex - 1] = 0;
                    event_pool->activethreadcount--;
                    timetodie = 1;
                    gen = ++event_pool->poller_gen;
                    list_for_each_entry(slot, &event_pool->poller_death,
                                        poller_death)
                    {
                        event_slot_ref(slot);
                    }

                    list_splice_init(&event_pool->poller_death,
                                     &poller_death_notify);
                    event_pool->poller_death_sliced = 1;
                    pthread_cond_broadcast(&event_pool->cond);
                }
            }
            pthread_mutex_unlock(&event_pool->mutex);
            if (timetodie) {
                list_for_each_entry(slot, &poller_death_notify, poller_death)
                {
                    slot->handler(slot->fd, 0, gen, slot->data, 0, 0, 0, 1);
                }

                pthread_mutex_lock(&event_pool->mutex);
                {
                    list_for_each_entry_safe(slot, tmp, &poller_death_notify,
                                             poller_death)
                    {
                        __event_slot_unref(event_pool, slot, slot->idx);
                    }

                    list_splice(&poller_death_notify,
                                &event_pool->poller_death);
                    event_pool->poller_death_sliced = 0;
                    pthread_cond_broadcast(&event_pool->cond);
                }
                pthread_mutex_unlock(&event_pool->mutex);

                gf_smsg("epoll", GF_LOG_INFO, 0, LG_MSG_EXITED_EPOLL_THREAD,
                        "index=%d", myindex, NULL);

                goto out;
            }
        }

        ret = epoll_wait(event_pool->fd, &event, 1, -1);

        if (ret == 0)
            /* timeout */
            continue;

        if (ret == -1 && errno == EINTR)
            /* sys call */
            continue;

        ret = event_dispatch_epoll_handler(event_pool, &event);
        if (ret) {
            gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_DISPATCH_HANDLER_FAILED,
                    NULL);
        }
    }
out:
    if (ev_data)
        GF_FREE(ev_data);
    return NULL;
}

/* Attempts to start the # of configured pollers, ensuring at least the first
 * is started in a joinable state */
static int
event_dispatch_epoll(struct event_pool *event_pool)
{
    int i = 0;
    pthread_t t_id;
    int pollercount = 0;
    int ret = -1;
    struct event_thread_data *ev_data = NULL;

    /* Start the configured number of pollers */
    pthread_mutex_lock(&event_pool->mutex);
    {
        pollercount = event_pool->eventthreadcount;

        /* Set to MAX if greater */
        if (pollercount > EVENT_MAX_THREADS)
            pollercount = EVENT_MAX_THREADS;

        /* Default pollers to 1 in case this is incorrectly set */
        if (pollercount <= 0)
            pollercount = 1;

        event_pool->activethreadcount++;

        for (i = 0; i < pollercount; i++) {
            ev_data = GF_MALLOC(sizeof(*ev_data), gf_common_mt_event_pool);
            if (!ev_data) {
                if (i == 0) {
                    /* Need to succeed creating 0'th
                     * thread, to joinable and wait */
                    break;
                } else {
                    /* Inability to create other threads
                     * are a lesser evil, and ignored */
                    continue;
                }
            }

            ev_data->event_pool = event_pool;
            ev_data->event_index = i + 1;

            ret = gf_thread_create(&t_id, NULL, event_dispatch_epoll_worker,
                                   ev_data, "epoll%03hx", i & 0x3ff);
            if (!ret) {
                event_pool->pollers[i] = t_id;

                /* mark all threads other than one in index 0
                 * as detachable. Errors can be ignored, they
                 * spend their time as zombies if not detched
                 * and the thread counts are decreased */
                if (i != 0)
                    pthread_detach(event_pool->pollers[i]);
            } else {
                gf_smsg("epoll", GF_LOG_WARNING, 0,
                        LG_MSG_START_EPOLL_THREAD_FAILED, "index=%d", i, NULL);
                if (i == 0) {
                    GF_FREE(ev_data);
                    break;
                } else {
                    GF_FREE(ev_data);
                    continue;
                }
            }
        }
    }
    pthread_mutex_unlock(&event_pool->mutex);

    /* Just wait for the first thread, that is created in a joinable state
     * and will never die, ensuring this function never returns */
    if (event_pool->pollers[0] != 0)
        pthread_join(event_pool->pollers[0], NULL);

    pthread_mutex_lock(&event_pool->mutex);
    {
        event_pool->activethreadcount--;
    }
    pthread_mutex_unlock(&event_pool->mutex);

    return ret;
}

/**
 * @param event_pool  event_pool on which fds of interest are registered for
 *                     events.
 *
 * @return  1 if at least one epoll worker thread is spawned, 0 otherwise
 *
 * NB This function SHOULD be called under event_pool->mutex.
 */

static int
event_pool_dispatched_unlocked(struct event_pool *event_pool)
{
    return (event_pool->pollers[0] != 0);
}

int
event_reconfigure_threads_epoll(struct event_pool *event_pool, int value)
{
    int i;
    int ret = 0;
    pthread_t t_id;
    int oldthreadcount;
    struct event_thread_data *ev_data = NULL;

    pthread_mutex_lock(&event_pool->mutex);
    {
        /* Reconfigure to 0 threads is allowed only in destroy mode */
        if (event_pool->destroy == 1) {
            value = 0;
        } else {
            /* Set to MAX if greater */
            if (value > EVENT_MAX_THREADS)
                value = EVENT_MAX_THREADS;

            /* Default pollers to 1 in case this is set incorrectly */
            if (value <= 0)
                value = 1;
        }

        oldthreadcount = event_pool->eventthreadcount;

        /* Start 'worker' threads as necessary only if event_dispatch()
         * was called before. If event_dispatch() was not called, there
         * will be no epoll 'worker' threads running yet. */

        if (event_pool_dispatched_unlocked(event_pool) &&
            (oldthreadcount < value)) {
            /* create more poll threads */
            for (i = oldthreadcount; i < value; i++) {
                /* Start a thread if the index at this location
                 * is a 0, so that the older thread is confirmed
                 * as dead */
                if (event_pool->pollers[i] == 0) {
                    ev_data = GF_CALLOC(1, sizeof(*ev_data),
                                        gf_common_mt_event_pool);
                    if (!ev_data) {
                        continue;
                    }

                    ev_data->event_pool = event_pool;
                    ev_data->event_index = i + 1;

                    ret = gf_thread_create(&t_id, NULL,
                                           event_dispatch_epoll_worker, ev_data,
                                           "epoll%03hx", i & 0x3ff);
                    if (ret) {
                        gf_smsg("epoll", GF_LOG_WARNING, 0,
                                LG_MSG_START_EPOLL_THREAD_FAILED, "index=%d", i,
                                NULL);
                        GF_FREE(ev_data);
                    } else {
                        pthread_detach(t_id);
                        event_pool->pollers[i] = t_id;
                    }
                }
            }
        }

        /* if value decreases, threads will terminate, themselves */
        event_pool->eventthreadcount = value;
    }
    pthread_mutex_unlock(&event_pool->mutex);

    return 0;
}

/* This function is the destructor for the event_pool data structure
 * Should be called only after poller_threads_destroy() is called,
 * else will lead to crashes.
 */
static int
event_pool_destroy_epoll(struct event_pool *event_pool)
{
    int ret = 0, i = 0, j = 0;
    struct event_slot_epoll *table = NULL;

    ret = sys_close(event_pool->fd);

    for (i = 0; i < EVENT_EPOLL_TABLES; i++) {
        if (event_pool->ereg[i]) {
            table = event_pool->ereg[i];
            event_pool->ereg[i] = NULL;
            for (j = 0; j < EVENT_EPOLL_SLOTS; j++) {
                if (table[j].fd != -1)
                    LOCK_DESTROY(&table[j].lock);
            }
            GF_FREE(table);
        }
    }

    pthread_mutex_destroy(&event_pool->mutex);
    pthread_cond_destroy(&event_pool->cond);

    GF_FREE(event_pool->evcache);
    GF_FREE(event_pool->reg);
    GF_FREE(event_pool);

    return ret;
}

static int
event_handled_epoll(struct event_pool *event_pool, int fd, int idx, int gen)
{
    struct event_slot_epoll *slot = NULL;
    struct epoll_event epoll_event = {
        0,
    };
    struct event_data *ev_data = (void *)&epoll_event.data;
    int ret = 0;

    slot = event_slot_get(event_pool, idx);
    if (!slot) {
        gf_smsg("epoll", GF_LOG_ERROR, 0, LG_MSG_SLOT_NOT_FOUND, "fd=%d", fd,
                "idx=%d", idx, NULL);
        return -1;
    }

    assert(slot->fd == fd);

    LOCK(&slot->lock);
    {
        slot->in_handler--;

        if (gen != slot->gen) {
            /* event_unregister() happened while we were
               in handler()
            */
            gf_msg_debug("epoll", 0,
                         "generation bumped on idx=%d"
                         " from gen=%d to slot->gen=%d, fd=%d, "
                         "slot->fd=%d",
                         idx, gen, slot->gen, fd, slot->fd);
            goto unlock;
        }

        /* This call also picks up the changes made by another
           thread calling event_select_on_epoll() while this
           thread was busy in handler()
        */
        else if (slot->in_handler == 0) {
            epoll_event.events = slot->events;
            ev_data->idx = idx;
            ev_data->gen = gen;

            ret = epoll_ctl(event_pool->fd, EPOLL_CTL_MOD, fd, &epoll_event);
        }
    }
unlock:
    UNLOCK(&slot->lock);

    event_slot_unref(event_pool, slot, idx);

    return ret;
}

struct event_ops event_ops_epoll = {
    .new = event_pool_new_epoll,
    .event_register = event_register_epoll,
    .event_select_on = event_select_on_epoll,
    .event_unregister = event_unregister_epoll,
    .event_unregister_close = event_unregister_close_epoll,
    .event_dispatch = event_dispatch_epoll,
    .event_reconfigure_threads = event_reconfigure_threads_epoll,
    .event_pool_destroy = event_pool_destroy_epoll,
    .event_handled = event_handled_epoll,
};

#endif
