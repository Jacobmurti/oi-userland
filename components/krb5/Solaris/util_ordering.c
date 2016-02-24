/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Copyright 1993 by OpenVision Technologies, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appears in all copies and
 * that both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of OpenVision not be used
 * in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission. OpenVision makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * OPENVISION DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL OPENVISION BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (c) 1999, 2015, Oracle and/or its affiliates. All rights reserved.
 */

/*
 * $Id: util_ordering.c 23457 2009-12-08 00:04:48Z tlyu $
 */

/*
 * functions to check sequence numbers for replay and sequencing
 */

#include "gssapiP_generic.h"
#include <string.h>

#define QUEUE_LENGTH 20

typedef struct _queue {
    int do_replay;
    int do_sequence;
    int start;
    int length;
    uint64_t firstnum;
    /* Stored as deltas from firstnum.  This way, the high bit won't
       overflow unless we've actually gone through 2**n messages, or
       gotten something *way* out of sequence.  */
    uint64_t elem[QUEUE_LENGTH];
    /* All ones for 64-bit sequence numbers; 32 ones for 32-bit
       sequence numbers.  */
    uint64_t mask;
} queue;

/* rep invariant:
 *  - the queue is a circular queue.  The first element (q->elem[q->start])
 * is the oldest.  The last element is the newest.
 */

#define QSIZE(q) (sizeof((q)->elem)/sizeof((q)->elem[0]))
#define QELEM(q,i) ((q)->elem[(i)%QSIZE(q)])

/*
 * Solaris Kerberos: begin
 * mask(max) is 2 ** 64 - 1, and half is 2 ** 63.
 * |-------------------------------|-----------------------------|
 * 0                              half                           mask
 *		   |-------------------------------|
 *                       half range ( 2 ** 63 )
 *
 * Here, the distance between n1 and n2 is used, if it falls
 * in the "half range", normal integer comparison is enough.
 *
 * If the distance is bigger than half of the range, one of them must
 * have passed the 'mask' point while the other one didn't.  In this
 * case, the result should be the reverse of normal comparison, i.e.
 * the smaller one is considered bigger.
 *
 * If we shift the smaller value by adding 'mask' to it,
 * the distance will be in half range again.
 *
 * The assumption is that the out of order event will not
 * happen too often.  If the distance is really bigger than half range,
 * (1 is expected, but half + 2 arrives) we really don't know if it's a
 * GAP token or an OLD token that wrapped.
 */
static int
after(uint64_t n1, uint64_t n2, uint64_t mask)
{
	int bigger;
	uint64_t diff;
	uint64_t half;

	/*
	 * rule 1: same number.
	 * This may be ambiguous, but the caller of this function,
	 * g_order_check already takes care of it.
	 */
	if (n1 == n2)
		return (0);

	half = 1 + (mask >> 1);

	if (n1 > n2) {
		diff = n1 - n2;
		bigger = 1;
	} else {
		diff = n2 - n1;
		bigger = 0;
	}

	/* rule 2: in the same half range, normal comparison is enough */
	if (diff < half)
		return bigger;

	n1 &= half;

	/* rule 3: different half, and n1 is on upper, n2 is bigger */
	/* rule 4: different half, and n1 is on lower, n1 is bigger */
	if (n1 != 0)
		return (0);

	return (1);
}
/* Solaris Kerberos: end */

static void
queue_insert(queue *q, int after, uint64_t seqnum)
{
    /* insert.  this is not the fastest way, but it's easy, and it's
       optimized for insert at end, which is the common case */
    int i;

    /* common case: at end, after == q->start+q->length-1 */

    /* move all the elements (after,last] up one slot */

    for (i=q->start+q->length-1; i>after; i--)
        QELEM(q,i+1) = QELEM(q,i);

    /* fill in slot after+1 */

    QELEM(q,after+1) = seqnum;

    /* Either increase the length by one, or move the starting point up
       one (deleting the first element, which got bashed above), as
       appropriate. */

    if (q->length == QSIZE(q)) {
        q->start++;
        if (q->start == QSIZE(q))
            q->start = 0;
    } else {
        q->length++;
    }
}

gss_int32
g_order_init(void **vqueue, uint64_t seqnum,
             int do_replay, int do_sequence, int wide_nums)
{
    queue *q;

    if ((q = (queue *) malloc(sizeof(queue))) == NULL)
        return(ENOMEM);

    /* This stops valgrind from complaining about writing uninitialized
       data if the caller exports the context and writes it to a file.
       We don't actually use those bytes at all, but valgrind still
       complains.  */
    (void) memset(q, 0xfe, sizeof(*q));

    q->do_replay = do_replay;
    q->do_sequence = do_sequence;
    q->mask = wide_nums ? ~(uint64_t)0 : 0xffffffffUL;

    q->start = 0;
    q->length = 1;
    q->firstnum = seqnum;
    q->elem[q->start] = ((uint64_t)0 - 1) & q->mask;

    *vqueue = (void *) q;
    return(0);
}

gss_int32
g_order_check(void **vqueue, uint64_t seqnum)
{
    queue *q;
    int i;
    uint64_t expected;

    q = (queue *) (*vqueue);

    if (!q->do_replay && !q->do_sequence)
        return(GSS_S_COMPLETE);

    /* All checks are done relative to the initial sequence number, to
       avoid (or at least put off) the pain of wrapping.  */
    seqnum -= q->firstnum;
    /* If we're only doing 32-bit values, adjust for that again.

       Note that this will probably be the wrong thing to if we get
       2**32 messages sent with 32-bit sequence numbers.  */
    seqnum &= q->mask;

    /* rule 1: expected sequence number */

    expected = (QELEM(q,q->start+q->length-1)+1) & q->mask;
    if (seqnum == expected) {
        queue_insert(q, q->start+q->length-1, seqnum);
        return(GSS_S_COMPLETE);
    }

    /* rule 2: > expected sequence number */

    /* Solaris Kerberos: using after */
    if (after(seqnum, expected, q->mask)) {
        queue_insert(q, q->start+q->length-1, seqnum);
        if (q->do_replay && !q->do_sequence)
            return(GSS_S_COMPLETE);
        else
            return(GSS_S_GAP_TOKEN);
    }

    /* rule 3: seqnum < seqnum(first) */

    /* Solaris Kerberos */
    if (after(QELEM(q,q->start), seqnum, q->mask)) {
        if (q->do_replay && !q->do_sequence)
            return(GSS_S_OLD_TOKEN);
        else
            return(GSS_S_UNSEQ_TOKEN);
    }

    /* rule 4+5: seqnum in [seqnum(first),seqnum(last)]  */

    else {
        if (seqnum == QELEM(q,q->start+q->length-1))
            return(GSS_S_DUPLICATE_TOKEN);

        for (i=q->start; i<q->start+q->length-1; i++) {
            if (seqnum == QELEM(q,i))
                return(GSS_S_DUPLICATE_TOKEN);
            /* Solaris Kerberos  */
            if (after(seqnum, QELEM(q,i), q->mask) &&
                after(QELEM(q,i+1), seqnum, q->mask)) {
                queue_insert(q, i, seqnum);
                if (q->do_replay && !q->do_sequence)
                    return(GSS_S_COMPLETE);
                else
                    return(GSS_S_UNSEQ_TOKEN);
            }
        }
    }

    /* this should never happen */
    return(GSS_S_FAILURE);
}

void
g_order_free(void **vqueue)
{
    queue *q;

    q = (queue *) (*vqueue);

    free(q);

    *vqueue = NULL;
}

/*
 * These support functions are for the serialization routines
 */
/* ARGSUSED */
gss_uint32
g_queue_size(void *vqueue, size_t *sizep)
{
    *sizep += sizeof(queue);
    return 0;
}

gss_uint32
g_queue_externalize(void *vqueue, unsigned char **buf, size_t *lenremain)
{
    if (*lenremain < sizeof(queue))
        return ENOMEM;
    (void) memcpy(*buf, vqueue, sizeof(queue));
    *buf += sizeof(queue);
    *lenremain -= sizeof(queue);

    return 0;
}

gss_uint32
g_queue_internalize(void **vqueue, unsigned char **buf, size_t *lenremain)
{
    void *q;

    if (*lenremain < sizeof(queue))
        return EINVAL;
    if ((q = malloc(sizeof(queue))) == 0)
        return ENOMEM;
    (void) memcpy(q, *buf, sizeof(queue));
    *buf += sizeof(queue);
    *lenremain -= sizeof(queue);
    *vqueue = q;
    return 0;
}
