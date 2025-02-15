/*
 * Copyright (c) 2016-2021 Apple Inc. All rights reserved.
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
 * The migration of flow queue between the different states is summarised in
 * the below state diagram. (RFC 8290)
 *
 * +-----------------+                +------------------+
 * |                 |     Empty      |                  |
 * |     Empty       |<---------------+       Old        +----+
 * |                 |                |                  |    |
 * +-------+---------+                +------------------+    |
 *         |                             ^            ^       |Credits
 *         |Arrival                      |            |       |Exhausted
 *         v                             |            |       |
 * +-----------------+                   |            |       |
 * |                 |      Empty or     |            |       |
 * |      New        +-------------------+            +-------+
 * |                 | Credits Exhausted
 * +-----------------+
 *
 * In this implementation of FQ-CODEL, flow queue is a dynamically allocated
 * object. An active flow queue goes through the above cycle of state
 * transitions very often. To avoid the cost of frequent flow queue object
 * allocation/free, this implementation retains the flow queue object in
 * [Empty] state on an Empty flow queue list with an active reference in flow
 * queue hash table. The flow queue objects on the Empty flow queue list have
 * an associated age and are purged accordingly.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/syslog.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/kauth.h>
#include <sys/sdt.h>
#include <kern/zalloc.h>
#include <netinet/in.h>

#include <net/classq/classq.h>
#include <net/classq/if_classq.h>
#include <net/pktsched/pktsched.h>
#include <net/pktsched/pktsched_fq_codel.h>
#include <net/classq/classq_fq_codel.h>

#include <netinet/tcp_var.h>

static uint32_t flowq_size;                     /* size of flowq */
static struct mcache *flowq_cache = NULL;       /* mcache for flowq */

#define FQ_ZONE_MAX     (32 * 1024)     /* across all interfaces */

#define DTYPE_NODROP    0       /* no drop */
#define DTYPE_FORCED    1       /* a "forced" drop */
#define DTYPE_EARLY     2       /* an "unforced" (early) drop */

static uint32_t pkt_compressor = 1;
#if (DEBUG || DEVELOPMENT)
SYSCTL_NODE(_net_classq, OID_AUTO, flow_q, CTLFLAG_RW | CTLFLAG_LOCKED,
    0, "FQ-CODEL parameters");

SYSCTL_UINT(_net_classq_flow_q, OID_AUTO, pkt_compressor,
    CTLFLAG_RW | CTLFLAG_LOCKED, &pkt_compressor, 0, "enable pkt compression");
#endif /* (DEBUG || DEVELOPMENT) */

void
fq_codel_init(void)
{
	if (flowq_cache != NULL) {
		return;
	}

	flowq_size = sizeof(fq_t);
	flowq_cache = mcache_create("fq.flowq", flowq_size, sizeof(uint64_t),
	    0, MCR_SLEEP);
	if (flowq_cache == NULL) {
		panic("%s: failed to allocate flowq_cache", __func__);
		/* NOTREACHED */
		__builtin_unreachable();
	}

	_CASSERT(AQM_KTRACE_AON_FLOW_HIGH_DELAY == 0x8300004);
	_CASSERT(AQM_KTRACE_AON_THROTTLE == 0x8300008);
	_CASSERT(AQM_KTRACE_AON_FLOW_OVERWHELMING == 0x830000c);
	_CASSERT(AQM_KTRACE_AON_FLOW_DQ_STALL == 0x8300010);

	_CASSERT(AQM_KTRACE_STATS_FLOW_ENQUEUE == 0x8310004);
	_CASSERT(AQM_KTRACE_STATS_FLOW_DEQUEUE == 0x8310008);
	_CASSERT(AQM_KTRACE_STATS_FLOW_CTL == 0x831000c);
	_CASSERT(AQM_KTRACE_STATS_FLOW_ALLOC == 0x8310010);
	_CASSERT(AQM_KTRACE_STATS_FLOW_DESTROY == 0x8310014);
}

void
fq_codel_reap_caches(boolean_t purge)
{
	mcache_reap_now(flowq_cache, purge);
}

fq_t *
fq_alloc(classq_pkt_type_t ptype)
{
	fq_t *fq = NULL;
	fq = mcache_alloc(flowq_cache, MCR_SLEEP);
	if (fq == NULL) {
		log(LOG_ERR, "%s: unable to allocate from flowq_cache\n", __func__);
		return NULL;
	}

	bzero(fq, flowq_size);
	if (ptype == QP_MBUF) {
		MBUFQ_INIT(&fq->fq_mbufq);
	}
#if SKYWALK
	else {
		VERIFY(ptype == QP_PACKET);
		KPKTQ_INIT(&fq->fq_kpktq);
	}
#endif /* SKYWALK */
	CLASSQ_PKT_INIT(&fq->fq_dq_head);
	CLASSQ_PKT_INIT(&fq->fq_dq_tail);
	fq->fq_in_dqlist = false;
	return fq;
}

void
fq_destroy(fq_t *fq, classq_pkt_type_t ptype)
{
	VERIFY(!fq->fq_in_dqlist);
	VERIFY(fq_empty(fq, ptype));
	VERIFY(!(fq->fq_flags & (FQF_NEW_FLOW | FQF_OLD_FLOW |
	    FQF_EMPTY_FLOW)));
	VERIFY(fq->fq_bytes == 0);
	mcache_free(flowq_cache, fq);
}

static inline void
fq_detect_dequeue_stall(fq_if_t *fqs, fq_t *flowq, fq_if_classq_t *fq_cl,
    u_int64_t *now)
{
	u_int64_t maxgetqtime, update_interval;
	if (FQ_IS_DELAY_HIGH(flowq) || flowq->fq_getqtime == 0 ||
	    fq_empty(flowq, fqs->fqs_ptype) ||
	    flowq->fq_bytes < FQ_MIN_FC_THRESHOLD_BYTES) {
		return;
	}

	update_interval = FQ_UPDATE_INTERVAL(flowq);
	maxgetqtime = flowq->fq_getqtime + update_interval;
	if ((*now) > maxgetqtime) {
		/*
		 * there was no dequeue in an update interval worth of
		 * time. It means that the queue is stalled.
		 */
		FQ_SET_DELAY_HIGH(flowq);
		fq_cl->fcl_stat.fcl_dequeue_stall++;
		os_log_error(OS_LOG_DEFAULT, "%s: dequeue stall num: %d, "
		    "scidx: %d, flow: 0x%x, iface: %s", __func__,
		    fq_cl->fcl_stat.fcl_dequeue_stall, flowq->fq_sc_index,
		    flowq->fq_flowhash, if_name(fqs->fqs_ifq->ifcq_ifp));
		KDBG(AQM_KTRACE_AON_FLOW_DQ_STALL, flowq->fq_flowhash,
		    AQM_KTRACE_FQ_GRP_SC_IDX(flowq), flowq->fq_bytes,
		    (*now) - flowq->fq_getqtime);
	}
}

void
fq_head_drop(fq_if_t *fqs, fq_t *fq)
{
	pktsched_pkt_t pkt;
	volatile uint32_t *pkt_flags;
	uint64_t *pkt_timestamp;
	struct ifclassq *ifq = fqs->fqs_ifq;

	_PKTSCHED_PKT_INIT(&pkt);
	fq_getq_flow_internal(fqs, fq, &pkt);
	if (pkt.pktsched_pkt_mbuf == NULL) {
		return;
	}

	pktsched_get_pkt_vars(&pkt, &pkt_flags, &pkt_timestamp, NULL, NULL,
	    NULL, NULL);

	*pkt_timestamp = 0;
	switch (pkt.pktsched_ptype) {
	case QP_MBUF:
		*pkt_flags &= ~PKTF_PRIV_GUARDED;
		break;
#if SKYWALK
	case QP_PACKET:
		/* sanity check */
		ASSERT((*pkt_flags & ~PKT_F_COMMON_MASK) == 0);
		break;
#endif /* SKYWALK */
	default:
		VERIFY(0);
		/* NOTREACHED */
		__builtin_unreachable();
	}

	IFCQ_DROP_ADD(ifq, 1, pktsched_get_pkt_len(&pkt));
	IFCQ_CONVERT_LOCK(ifq);
	pktsched_free_pkt(&pkt);
}


static int
fq_compressor(fq_if_t *fqs, fq_t *fq, fq_if_classq_t *fq_cl,
    pktsched_pkt_t *pkt)
{
	classq_pkt_type_t ptype = fqs->fqs_ptype;
	uint32_t comp_gencnt = 0;
	uint64_t *pkt_timestamp;
	uint64_t old_timestamp = 0;
	uint32_t old_pktlen = 0;
	struct ifclassq *ifq = fqs->fqs_ifq;

	if (__improbable(pkt_compressor == 0)) {
		return 0;
	}

	pktsched_get_pkt_vars(pkt, NULL, &pkt_timestamp, NULL, NULL, NULL,
	    &comp_gencnt);

	if (comp_gencnt == 0) {
		return 0;
	}

	fq_cl->fcl_stat.fcl_pkts_compressible++;

	if (fq_empty(fq, fqs->fqs_ptype)) {
		return 0;
	}

	if (ptype == QP_MBUF) {
		struct mbuf *m = MBUFQ_LAST(&fq->fq_mbufq);

		if (comp_gencnt != m->m_pkthdr.comp_gencnt) {
			return 0;
		}

		/* If we got until here, we should merge/replace the segment */
		MBUFQ_REMOVE(&fq->fq_mbufq, m);
		old_pktlen = m_pktlen(m);
		old_timestamp = m->m_pkthdr.pkt_timestamp;

		IFCQ_CONVERT_LOCK(fqs->fqs_ifq);
		m_freem(m);
	}
#if SKYWALK
	else {
		struct __kern_packet *kpkt = KPKTQ_LAST(&fq->fq_kpktq);

		if (comp_gencnt != kpkt->pkt_comp_gencnt) {
			return 0;
		}

		/* If we got until here, we should merge/replace the segment */
		KPKTQ_REMOVE(&fq->fq_kpktq, kpkt);
		old_pktlen = kpkt->pkt_length;
		old_timestamp = kpkt->pkt_timestamp;

		IFCQ_CONVERT_LOCK(fqs->fqs_ifq);
		pp_free_packet(*(struct kern_pbufpool **)(uintptr_t)&
		    (((struct __kern_quantum *)kpkt)->qum_pp),
		    (uint64_t)kpkt);
	}
#endif /* SKYWALK */

	fq->fq_bytes -= old_pktlen;
	fq_cl->fcl_stat.fcl_byte_cnt -= old_pktlen;
	fq_cl->fcl_stat.fcl_pkt_cnt--;
	IFCQ_DEC_LEN(ifq);
	IFCQ_DEC_BYTES(ifq, old_pktlen);

	FQ_GRP_DEC_LEN(fq);
	FQ_GRP_DEC_BYTES(fq, old_pktlen);

	*pkt_timestamp = old_timestamp;

	return CLASSQEQ_COMPRESSED;
}

int
fq_addq(fq_if_t *fqs, fq_if_group_t *fq_grp, pktsched_pkt_t *pkt,
    fq_if_classq_t *fq_cl)
{
	int droptype = DTYPE_NODROP, fc_adv = 0, ret = CLASSQEQ_SUCCESS;
	u_int64_t now;
	fq_t *fq = NULL;
	uint64_t *pkt_timestamp;
	volatile uint32_t *pkt_flags;
	uint32_t pkt_flowid, cnt;
	uint8_t pkt_proto, pkt_flowsrc;
	fq_tfc_type_t tfc_type = FQ_TFC_C;

	cnt = pkt->pktsched_pcnt;
	pktsched_get_pkt_vars(pkt, &pkt_flags, &pkt_timestamp, &pkt_flowid,
	    &pkt_flowsrc, &pkt_proto, NULL);

	/*
	 * XXX Not walking the chain to set this flag on every packet.
	 * This flag is only used for debugging. Nothing is affected if it's
	 * not set.
	 */
	switch (pkt->pktsched_ptype) {
	case QP_MBUF:
		/* See comments in <rdar://problem/14040693> */
		VERIFY(!(*pkt_flags & PKTF_PRIV_GUARDED));
		*pkt_flags |= PKTF_PRIV_GUARDED;
		break;
#if SKYWALK
	case QP_PACKET:
		/* sanity check */
		ASSERT((*pkt_flags & ~PKT_F_COMMON_MASK) == 0);
		break;
#endif /* SKYWALK */
	default:
		VERIFY(0);
		/* NOTREACHED */
		__builtin_unreachable();
	}

	/*
	 * Timestamps for every packet must be set prior to entering this path.
	 */
	now = *pkt_timestamp;
	ASSERT(now > 0);

	/* find the flowq for this packet */
	fq = fq_if_hash_pkt(fqs, fq_grp, pkt_flowid, pktsched_get_pkt_svc(pkt),
	    now, true, tfc_type);
	if (__improbable(fq == NULL)) {
		DTRACE_IP1(memfail__drop, fq_if_t *, fqs);
		/* drop the packet if we could not allocate a flow queue */
		fq_cl->fcl_stat.fcl_drop_memfailure += cnt;
		return CLASSQEQ_DROP;
	}
	VERIFY(fq->fq_group == fq_grp);
	VERIFY(fqs->fqs_ptype == pkt->pktsched_ptype);

	KDBG(AQM_KTRACE_STATS_FLOW_ENQUEUE, fq->fq_flowhash,
	    AQM_KTRACE_FQ_GRP_SC_IDX(fq),
	    fq->fq_bytes, pktsched_get_pkt_len(pkt));

	fq_detect_dequeue_stall(fqs, fq, fq_cl, &now);

	if (__improbable(FQ_IS_DELAY_HIGH(fq) || FQ_IS_OVERWHELMING(fq))) {
		if ((fq->fq_flags & FQF_FLOWCTL_CAPABLE) &&
		    (*pkt_flags & PKTF_FLOW_ADV)) {
			fc_adv = 1;
			/*
			 * If the flow is suspended or it is not
			 * TCP/QUIC, drop the chain.
			 */
			if ((pkt_proto != IPPROTO_TCP) &&
			    (pkt_proto != IPPROTO_QUIC)) {
				droptype = DTYPE_EARLY;
				fq_cl->fcl_stat.fcl_drop_early += cnt;
				IFCQ_DROP_ADD(fqs->fqs_ifq, cnt, pktsched_get_pkt_len(pkt));
			}
			DTRACE_IP6(flow__adv, fq_if_t *, fqs,
			    fq_if_classq_t *, fq_cl, fq_t *, fq,
			    int, droptype, pktsched_pkt_t *, pkt,
			    uint32_t, cnt);
		} else {
			/*
			 * Need to drop packets to make room for the new
			 * ones. Try to drop from the head of the queue
			 * instead of the latest packets.
			 */
			if (!fq_empty(fq, fqs->fqs_ptype)) {
				uint32_t i;

				for (i = 0; i < cnt; i++) {
					fq_head_drop(fqs, fq);
				}
				droptype = DTYPE_NODROP;
			} else {
				droptype = DTYPE_EARLY;
			}
			fq_cl->fcl_stat.fcl_drop_early += cnt;

			DTRACE_IP6(no__flow__adv, fq_if_t *, fqs,
			    fq_if_classq_t *, fq_cl, fq_t *, fq,
			    int, droptype, pktsched_pkt_t *, pkt,
			    uint32_t, cnt);
		}
	}

	/* Set the return code correctly */
	if (__improbable(fc_adv == 1 && droptype != DTYPE_FORCED)) {
		if (fq_if_add_fcentry(fqs, pkt, pkt_flowsrc, fq, fq_cl)) {
			fq->fq_flags |= FQF_FLOWCTL_ON;
			/* deliver flow control advisory error */
			if (droptype == DTYPE_NODROP) {
				ret = CLASSQEQ_SUCCESS_FC;
			} else {
				/* dropped due to flow control */
				ret = CLASSQEQ_DROP_FC;
			}
		} else {
			/*
			 * if we could not flow control the flow, it is
			 * better to drop
			 */
			droptype = DTYPE_FORCED;
			ret = CLASSQEQ_DROP_FC;
			fq_cl->fcl_stat.fcl_flow_control_fail++;
		}
		DTRACE_IP3(fc__ret, fq_if_t *, fqs, int, droptype, int, ret);
	}

	/*
	 * If the queue length hits the queue limit, drop a chain with the
	 * same number of packets from the front of the queue for a flow with
	 * maximum number of bytes. This will penalize heavy and unresponsive
	 * flows. It will also avoid a tail drop.
	 */
	if (__improbable(droptype == DTYPE_NODROP &&
	    fq_if_at_drop_limit(fqs))) {
		uint32_t i;

		if (fqs->fqs_large_flow == fq) {
			/*
			 * Drop from the head of the current fq. Since a
			 * new packet will be added to the tail, it is ok
			 * to leave fq in place.
			 */
			DTRACE_IP5(large__flow, fq_if_t *, fqs,
			    fq_if_classq_t *, fq_cl, fq_t *, fq,
			    pktsched_pkt_t *, pkt, uint32_t, cnt);

			for (i = 0; i < cnt; i++) {
				fq_head_drop(fqs, fq);
			}
			fq_cl->fcl_stat.fcl_drop_overflow += cnt;

			/*
			 * TCP and QUIC will react to the loss of those head dropped pkts
			 * and adjust send rate.
			 */
			if ((fq->fq_flags & FQF_FLOWCTL_CAPABLE) &&
			    (*pkt_flags & PKTF_FLOW_ADV) &&
			    (pkt_proto != IPPROTO_TCP) &&
			    (pkt_proto != IPPROTO_QUIC)) {
				if (fq_if_add_fcentry(fqs, pkt, pkt_flowsrc, fq, fq_cl)) {
					fq->fq_flags |= FQF_FLOWCTL_ON;
					FQ_SET_OVERWHELMING(fq);
					fq_cl->fcl_stat.fcl_overwhelming++;
					/* deliver flow control advisory error */
					ret = CLASSQEQ_SUCCESS_FC;
				}
			}
		} else {
			if (fqs->fqs_large_flow == NULL) {
				droptype = DTYPE_FORCED;
				fq_cl->fcl_stat.fcl_drop_overflow += cnt;
				ret = CLASSQEQ_DROP;

				DTRACE_IP5(no__large__flow, fq_if_t *, fqs,
				    fq_if_classq_t *, fq_cl, fq_t *, fq,
				    pktsched_pkt_t *, pkt, uint32_t, cnt);

				/*
				 * if this fq was freshly created and there
				 * is nothing to enqueue, move it to empty list
				 */
				if (fq_empty(fq, fqs->fqs_ptype) &&
				    !(fq->fq_flags & (FQF_NEW_FLOW |
				    FQF_OLD_FLOW))) {
					fq_if_move_to_empty_flow(fqs, fq_cl,
					    fq, now);
					fq = NULL;
				}
			} else {
				DTRACE_IP5(different__large__flow,
				    fq_if_t *, fqs, fq_if_classq_t *, fq_cl,
				    fq_t *, fq, pktsched_pkt_t *, pkt,
				    uint32_t, cnt);

				for (i = 0; i < cnt; i++) {
					fq_if_drop_packet(fqs, now);
				}
			}
		}
	}

	if (__probable(droptype == DTYPE_NODROP)) {
		uint32_t chain_len = pktsched_get_pkt_len(pkt);

		/*
		 * We do not compress if we are enqueuing a chain.
		 * Traversing the chain to look for acks would defeat the
		 * purpose of batch enqueueing.
		 */
		if (cnt == 1) {
			ret = fq_compressor(fqs, fq, fq_cl, pkt);
			if (ret != CLASSQEQ_COMPRESSED) {
				ret = CLASSQEQ_SUCCESS;
			} else {
				fq_cl->fcl_stat.fcl_pkts_compressed++;
			}
		}
		DTRACE_IP5(fq_enqueue, fq_if_t *, fqs, fq_if_classq_t *, fq_cl,
		    fq_t *, fq, pktsched_pkt_t *, pkt, uint32_t, cnt);
		fq_enqueue(fq, pkt->pktsched_pkt, pkt->pktsched_tail, cnt,
		    pkt->pktsched_ptype);

		fq->fq_bytes += chain_len;
		fq_cl->fcl_stat.fcl_byte_cnt += chain_len;
		fq_cl->fcl_stat.fcl_pkt_cnt += cnt;

		/*
		 * check if this queue will qualify to be the next
		 * victim queue
		 */
		fq_if_is_flow_heavy(fqs, fq);
	} else {
		DTRACE_IP3(fq_drop, fq_if_t *, fqs, int, droptype, int, ret);
		return (ret != CLASSQEQ_SUCCESS) ? ret : CLASSQEQ_DROP;
	}

	/*
	 * If the queue is not currently active, add it to the end of new
	 * flows list for that service class.
	 */
	if ((fq->fq_flags & (FQF_NEW_FLOW | FQF_OLD_FLOW)) == 0) {
		VERIFY(STAILQ_NEXT(fq, fq_actlink) == NULL);
		STAILQ_INSERT_TAIL(&fq_cl->fcl_new_flows, fq, fq_actlink);
		fq->fq_flags |= FQF_NEW_FLOW;

		fq_cl->fcl_stat.fcl_newflows_cnt++;

		fq->fq_deficit = fq_cl->fcl_quantum;
	}
	return ret;
}

void
fq_getq_flow_internal(fq_if_t *fqs, fq_t *fq, pktsched_pkt_t *pkt)
{
	classq_pkt_t p = CLASSQ_PKT_INITIALIZER(p);
	uint32_t plen;
	fq_if_classq_t *fq_cl;
	struct ifclassq *ifq = fqs->fqs_ifq;

	fq_dequeue(fq, &p, fqs->fqs_ptype);
	if (p.cp_ptype == QP_INVALID) {
		VERIFY(p.cp_mbuf == NULL);
		return;
	}

	pktsched_pkt_encap(pkt, &p);
	plen = pktsched_get_pkt_len(pkt);

	VERIFY(fq->fq_bytes >= plen);
	fq->fq_bytes -= plen;

	fq_cl = &FQ_CLASSQ(fq);
	fq_cl->fcl_stat.fcl_byte_cnt -= plen;
	fq_cl->fcl_stat.fcl_pkt_cnt--;
	IFCQ_DEC_LEN(ifq);
	IFCQ_DEC_BYTES(ifq, plen);

	FQ_GRP_DEC_LEN(fq);
	FQ_GRP_DEC_BYTES(fq, plen);

	/* Reset getqtime so that we don't count idle times */
	if (fq_empty(fq, fqs->fqs_ptype)) {
		fq->fq_getqtime = 0;
	}
}

void
fq_getq_flow(fq_if_t *fqs, fq_t *fq, pktsched_pkt_t *pkt, uint64_t now)
{
	fq_if_classq_t *fq_cl;
	int64_t qdelay = 0;
	volatile uint32_t *pkt_flags;
	uint64_t *pkt_timestamp;

	fq_getq_flow_internal(fqs, fq, pkt);
	if (pkt->pktsched_ptype == QP_INVALID) {
		VERIFY(pkt->pktsched_pkt_mbuf == NULL);
		return;
	}

	pktsched_get_pkt_vars(pkt, &pkt_flags, &pkt_timestamp, NULL, NULL,
	    NULL, NULL);

	/* this will compute qdelay in nanoseconds */
	if (now > *pkt_timestamp) {
		qdelay = now - *pkt_timestamp;
	}
	fq_cl = &FQ_CLASSQ(fq);

	if (fq->fq_min_qdelay == 0 ||
	    (qdelay > 0 && (u_int64_t)qdelay < fq->fq_min_qdelay)) {
		fq->fq_min_qdelay = qdelay;
	}

	/* Update min/max/avg qdelay for the respective class */
	if (fq_cl->fcl_stat.fcl_min_qdelay == 0 ||
	    (qdelay > 0 && (u_int64_t)qdelay < fq_cl->fcl_stat.fcl_min_qdelay)) {
		fq_cl->fcl_stat.fcl_min_qdelay = qdelay;
	}

	if (fq_cl->fcl_stat.fcl_max_qdelay == 0 ||
	    (qdelay > 0 && (u_int64_t)qdelay > fq_cl->fcl_stat.fcl_max_qdelay)) {
		fq_cl->fcl_stat.fcl_max_qdelay = qdelay;
	}

	uint64_t num_dequeues = fq_cl->fcl_stat.fcl_dequeue;

	if (num_dequeues == 0) {
		fq_cl->fcl_stat.fcl_avg_qdelay = qdelay;
	} else if (qdelay > 0) {
		uint64_t res = 0;
		if (os_add_overflow(num_dequeues, 1, &res)) {
			/* Reset the dequeue num and dequeue bytes */
			fq_cl->fcl_stat.fcl_dequeue = num_dequeues = 0;
			fq_cl->fcl_stat.fcl_dequeue_bytes = 0;
			fq_cl->fcl_stat.fcl_avg_qdelay = qdelay;
			os_log_info(OS_LOG_DEFAULT, "%s: dequeue num overflow, "
			    "flow: 0x%x, iface: %s", __func__, fq->fq_flowhash,
			    if_name(fqs->fqs_ifq->ifcq_ifp));
		} else {
			uint64_t product = 0;
			if (os_mul_overflow(fq_cl->fcl_stat.fcl_avg_qdelay,
			    num_dequeues, &product) || os_add_overflow(product, qdelay, &res)) {
				fq_cl->fcl_stat.fcl_avg_qdelay = qdelay;
			} else {
				fq_cl->fcl_stat.fcl_avg_qdelay = res /
				    (num_dequeues + 1);
			}
		}
	}

	if (now >= fq->fq_updatetime) {
		if (fq->fq_min_qdelay > FQ_TARGET_DELAY(fq)) {
			if (!FQ_IS_DELAY_HIGH(fq)) {
				FQ_SET_DELAY_HIGH(fq);
				os_log_error(OS_LOG_DEFAULT,
				    "%s: high delay idx: %d, %llu, flow: 0x%x, "
				    "iface: %s", __func__, fq->fq_sc_index,
				    fq->fq_min_qdelay, fq->fq_flowhash,
				    if_name(fqs->fqs_ifq->ifcq_ifp));
			}
		} else {
			FQ_CLEAR_DELAY_HIGH(fq);
		}
		/* Reset measured queue delay and update time */
		fq->fq_updatetime = now + FQ_UPDATE_INTERVAL(fq);
		fq->fq_min_qdelay = 0;
	}
	if (fqs->fqs_large_flow != fq || !fq_if_almost_at_drop_limit(fqs)) {
		FQ_CLEAR_OVERWHELMING(fq);
	}
	if (!FQ_IS_DELAY_HIGH(fq) || fq_empty(fq, fqs->fqs_ptype)) {
		FQ_CLEAR_DELAY_HIGH(fq);
	}

	if ((fq->fq_flags & FQF_FLOWCTL_ON) &&
	    !FQ_IS_DELAY_HIGH(fq) && !FQ_IS_OVERWHELMING(fq)) {
		fq_if_flow_feedback(fqs, fq, fq_cl);
	}

	if (fq_empty(fq, fqs->fqs_ptype)) {
		/* Reset getqtime so that we don't count idle times */
		fq->fq_getqtime = 0;
	} else {
		fq->fq_getqtime = now;
	}
	fq_if_is_flow_heavy(fqs, fq);

	*pkt_timestamp = 0;
	switch (pkt->pktsched_ptype) {
	case QP_MBUF:
		*pkt_flags &= ~PKTF_PRIV_GUARDED;
		break;
#if SKYWALK
	case QP_PACKET:
		/* sanity check */
		ASSERT((*pkt_flags & ~PKT_F_COMMON_MASK) == 0);
		break;
#endif /* SKYWALK */
	default:
		VERIFY(0);
		/* NOTREACHED */
		__builtin_unreachable();
	}
}
