/*
 * Copyright (c) 2000-2020 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 2000 Apple Computer, Inc.  All rights reserved.
 *
 * HISTORY
 *
 * 29 June 2000 (debo)
 *  Created.
 */

#include <mach/mach_types.h>
#include <mach/mach_traps.h>
#include <mach/mach_port_server.h>

#include <mach/mk_timer.h>

#include <ipc/ipc_space.h>

#include <kern/lock_group.h>
#include <kern/thread_call.h>
#include <ipc/ipc_kmsg.h>

struct mk_timer {
	decl_simple_lock_data(, lock);
	thread_call_data_t      mkt_thread_call;
	uint32_t                is_dead:1,
	    is_armed:1;
	int                     active;
	ipc_port_t              port;
};

static ZONE_DEFINE_TYPE(mk_timer_zone, "mk_timer",
    struct mk_timer, ZC_ZFREE_CLEARMEM);

static void mk_timer_port_destroy(ipc_port_t);
static void mk_timer_expire(void *p0, void *p1);

IPC_KOBJECT_DEFINE(IKOT_TIMER,
    .iko_op_destroy = mk_timer_port_destroy);

__abortlike
static void
ipc_kobject_mktimer_require_panic(
	ipc_port_t                  port)
{
	panic("port %p / mktimer %p: circularity check failed",
	    port, ipc_kobject_get_raw(port, IKOT_TIMER));
}

void
ipc_kobject_mktimer_require_locked(
	ipc_port_t                  port)
{
	struct mk_timer *timer;

	timer = ipc_kobject_get_locked(port, IKOT_TIMER);
	if (timer->port != port) {
		ipc_kobject_mktimer_require_panic(port);
	}
}

mach_port_name_t
mk_timer_create_trap(
	__unused struct mk_timer_create_trap_args *args)
{
	struct mk_timer*      timer;
	ipc_space_t           myspace = current_space();
	mach_port_name_t      name = MACH_PORT_NULL;
	ipc_port_init_flags_t init_flags;
	ipc_port_t            port;
	kern_return_t         result;
	ipc_kmsg_t            kmsg;

	/* Allocate and initialize local state of a timer object */
	timer = zalloc_flags(mk_timer_zone, Z_ZERO | Z_WAITOK | Z_NOFAIL);
	simple_lock_init(&timer->lock, 0);
	thread_call_setup(&timer->mkt_thread_call, mk_timer_expire, timer);

	/* Pre-allocate a kmsg for the timer messages */
	kmsg = ipc_kmsg_alloc(sizeof(mk_timer_expire_msg_t), 0, 0,
	    IPC_KMSG_ALLOC_KERNEL | IPC_KMSG_ALLOC_ZERO |
	    IPC_KMSG_ALLOC_SAVED | IPC_KMSG_ALLOC_NOFAIL);

	init_flags = IPC_PORT_INIT_MESSAGE_QUEUE;
	result = ipc_port_alloc(myspace, init_flags, &name, &port);
	if (result != KERN_SUCCESS) {
		zfree(mk_timer_zone, timer);
		ipc_kmsg_free(kmsg);
		return MACH_PORT_NULL;
	}

	/* Associate the pre-allocated kmsg with the port */
	ipc_kmsg_set_prealloc(kmsg, port);

	/* port locked, receive right at user-space */
	ipc_kobject_upgrade_mktimer_locked(port, (ipc_kobject_t)timer);

	/* make a (naked) send right for the timer to keep */
	timer->port = ipc_port_make_send_any_locked(port);

	ip_mq_unlock(port);

	return name;
}

static void
mk_timer_port_destroy(
	ipc_port_t                      port)
{
	struct mk_timer *timer = NULL;

	timer = ipc_kobject_disable(port, IKOT_TIMER);

	simple_lock(&timer->lock, LCK_GRP_NULL);

	if (thread_call_cancel(&timer->mkt_thread_call)) {
		timer->active--;
	}
	timer->is_armed = FALSE;

	timer->is_dead = TRUE;
	if (timer->active == 0) {
		simple_unlock(&timer->lock);
		zfree(mk_timer_zone, timer);

		ipc_port_release_send(port);
		return;
	}

	simple_unlock(&timer->lock);
}

static void
mk_timer_expire(
	void                    *p0,
	__unused void           *p1)
{
	struct mk_timer* timer = p0;

	simple_lock(&timer->lock, LCK_GRP_NULL);

	if (timer->active > 1) {
		timer->active--;
		simple_unlock(&timer->lock);
		return;
	}

	ipc_port_t port = timer->port;
	assert(port != IP_NULL);
	assert(timer->active == 1);

	while (timer->is_armed && timer->active == 1) {
		mk_timer_expire_msg_t           msg;

		timer->is_armed = FALSE;
		simple_unlock(&timer->lock);

		msg.header.msgh_bits =
		    MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, 0);
		msg.header.msgh_remote_port = port;
		msg.header.msgh_local_port = MACH_PORT_NULL;
		msg.header.msgh_voucher_port = MACH_PORT_NULL;
		msg.header.msgh_id = 0;

		msg.unused[0] = msg.unused[1] = msg.unused[2] = 0;

		(void) mach_msg_send_from_kernel_proper(&msg.header, sizeof(msg));

		simple_lock(&timer->lock, LCK_GRP_NULL);
	}

	if (--timer->active == 0 && timer->is_dead) {
		simple_unlock(&timer->lock);
		zfree(mk_timer_zone, timer);

		ipc_port_release_send(port);
		return;
	}

	simple_unlock(&timer->lock);
}

/*
 * mk_timer_destroy_trap: Destroy the Mach port associated with a timer
 *
 * Parameters:  args                     User argument descriptor (see below)
 *
 * Indirect:     args->name               Mach port name
 *
 *
 * Returns:        0                      Success
 *                !0                      Not success
 *
 */
kern_return_t
mk_timer_destroy_trap(
	struct mk_timer_destroy_trap_args *args)
{
	mach_port_name_t        name = args->name;
	ipc_space_t                     myspace = current_space();
	ipc_port_t                      port;
	kern_return_t           result;

	result = ipc_port_translate_receive(myspace, name, &port);
	if (result != KERN_SUCCESS) {
		return result;
	}

	if (ip_kotype(port) == IKOT_TIMER) {
		ip_mq_unlock(port);
		/* TODO: this should be mach_port_mod_refs */
		result = mach_port_destroy(myspace, name);
	} else {
		ip_mq_unlock(port);
		result = KERN_INVALID_ARGUMENT;
	}

	return result;
}

/*
 * mk_timer_arm_trap: Start (arm) a timer
 *
 * Parameters:  args                     User argument descriptor (see below)
 *
 * Indirect:     args->name               Mach port name
 *               args->expire_time        Time when timer expires
 *
 *
 * Returns:        0                      Success
 *                !0                      Not success
 *
 */

static kern_return_t
mk_timer_arm_trap_internal(mach_port_name_t name, uint64_t expire_time, uint64_t mk_leeway, uint64_t mk_timer_flags)
{
	struct mk_timer*                timer;
	ipc_space_t                     myspace = current_space();
	ipc_port_t                      port;
	kern_return_t                   result;

	result = ipc_port_translate_receive(myspace, name, &port);
	if (result != KERN_SUCCESS) {
		return result;
	}

	timer = ipc_kobject_get_locked(port, IKOT_TIMER);

	if (timer) {

		simple_lock(&timer->lock, LCK_GRP_NULL);
		assert(timer->port == port);
		ip_mq_unlock(port);

		if (!timer->is_dead) {
			timer->is_armed = TRUE;

			if (expire_time > mach_absolute_time()) {
				uint32_t tcflags = THREAD_CALL_DELAY_USER_NORMAL;

				if (mk_timer_flags & MK_TIMER_CRITICAL) {
					tcflags = THREAD_CALL_DELAY_USER_CRITICAL;
				}

				if (mk_leeway != 0) {
					tcflags |= THREAD_CALL_DELAY_LEEWAY;
				}

				if (!thread_call_enter_delayed_with_leeway(
					    &timer->mkt_thread_call, NULL,
					    expire_time, mk_leeway, tcflags)) {
					timer->active++;
				}
			} else {
				if (!thread_call_enter1(&timer->mkt_thread_call, NULL)) {
					timer->active++;
				}
			}
		}

		simple_unlock(&timer->lock);
	} else {
		ip_mq_unlock(port);
		result = KERN_INVALID_ARGUMENT;
	}
	return result;
}

kern_return_t
mk_timer_arm_trap(struct mk_timer_arm_trap_args *args)
{
	return mk_timer_arm_trap_internal(args->name, args->expire_time, 0, MK_TIMER_NORMAL);
}

kern_return_t
mk_timer_arm_leeway_trap(struct mk_timer_arm_leeway_trap_args *args)
{
	return mk_timer_arm_trap_internal(args->name, args->expire_time, args->mk_leeway, args->mk_timer_flags);
}

/*
 * mk_timer_cancel_trap: Cancel a timer
 *
 * Parameters:  args                     User argument descriptor (see below)
 *
 * Indirect:     args->name               Mach port name
 *               args->result_time        The armed time of the cancelled timer (return value)
 *
 *
 * Returns:        0                      Success
 *                !0                      Not success
 *
 */
kern_return_t
mk_timer_cancel_trap(
	struct mk_timer_cancel_trap_args *args)
{
	mach_port_name_t        name = args->name;
	mach_vm_address_t       result_time_addr = args->result_time;
	uint64_t                        armed_time = 0;
	struct mk_timer*                timer;
	ipc_space_t                     myspace = current_space();
	ipc_port_t                      port;
	kern_return_t           result;

	result = ipc_port_translate_receive(myspace, name, &port);
	if (result != KERN_SUCCESS) {
		return result;
	}

	timer = ipc_kobject_get_locked(port, IKOT_TIMER);
	if (timer != NULL) {
		simple_lock(&timer->lock, LCK_GRP_NULL);
		assert(timer->port == port);
		ip_mq_unlock(port);

		if (timer->is_armed) {
			armed_time = thread_call_get_armed_deadline(&timer->mkt_thread_call);
			if (thread_call_cancel(&timer->mkt_thread_call)) {
				timer->active--;
			}
			timer->is_armed = FALSE;
		}

		simple_unlock(&timer->lock);
	} else {
		ip_mq_unlock(port);
		result = KERN_INVALID_ARGUMENT;
	}

	if (result == KERN_SUCCESS && result_time_addr != 0) {
		if (copyout((void *)&armed_time, result_time_addr, sizeof(armed_time)) != 0) {
			result = KERN_FAILURE;
		}
	}

	return result;
}
