/*-
 * Copyright (c) 1983 Regents of the University of California.
 * Copyright (c) 2015 SRI International
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(LIBC_SCCS) && !defined(lint)
/*static char *sccsid = "from: @(#)malloc.c	5.11 (Berkeley) 2/23/91";*/
static char *rcsid = "$FreeBSD$";
#endif /* LIBC_SCCS and not lint */

/*
 * malloc.c (Caltech) 2/21/82
 * Chris Kingsley, kingsley@cit-20.
 *
 * This is a very fast storage allocator.  It allocates blocks of a small
 * number of different sizes, and keeps free lists of each size.  Blocks that
 * don't exactly fit are passed up to the next larger size.  In this
 * implementation, the available sizes are 2^n-4 (or 2^n-10) bytes long.
 * This is designed for use in a virtual memory environment.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <machine/cheri.h>
#include <machine/cheric.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

union overhead;
static void morecore(int);

/*
 * Pre-allocate mmap'ed pages
 */
#define	NPOOLPAGES	(32*1024/pagesz)
static caddr_t		pagepool_start, pagepool_end;
static int		morepages(int n);

/*
 * The overhead on a block is at least 4 bytes.  When free, this space
 * contains a pointer to the next free block, and the bottom two bits must
 * be zero.  When in use, the first byte is set to MAGIC, and the second
 * byte is the size index.  The remaining bytes are for alignment.
 * If range checking is enabled then a second word holds the size of the
 * requested block, less 1, rounded up to a multiple of sizeof(RMAGIC).
 * The order of elements is critical: ov_magic must overlay the low order
 * bits of ov_next, and ov_magic can not be a valid ov_next bit pattern.
 */
union	overhead {
	union	overhead *ov_next;	/* when free */
	struct {
		u_char	ovu_magic;	/* magic number */
		u_char	ovu_index;	/* bucket # */
#ifdef RCHECK
		u_short	ovu_rmagic;	/* range magic number */
		u_int	ovu_size;	/* actual block size */
#endif
	} ovu;
#define	ov_magic	ovu.ovu_magic
#define	ov_index	ovu.ovu_index
#define	ov_rmagic	ovu.ovu_rmagic
#define	ov_size		ovu.ovu_size
};

#define	MAGIC		0xef		/* magic # on accounting info */
#define RMAGIC		0x5555		/* magic # on range info */

#ifdef RCHECK
#define	RSLOP		sizeof (u_short)
#else
#define	RSLOP		0
#endif

/*
 * nextf[i] is the pointer to the next free block of size 2^(i+3).  The
 * smallest allocatable block is 8 bytes.  The overhead information
 * precedes the data area returned to the user.
 */
#define	NBUCKETS 30
static	union overhead *nextf[NBUCKETS];

static	int pagesz;			/* page size */
static	int pagebucket;			/* page size bucket */

#ifdef MSTATS
/*
 * nmalloc[i] is the difference between the number of mallocs and frees
 * for a given block size.
 */
static	u_int nmalloc[NBUCKETS];
#include <stdio.h>
#endif

#if defined(MALLOC_DEBUG) || defined(RCHECK)
#define	ASSERT(p)   if (!(p)) botch("p")
#include <stdio.h>
static void
botch(char *s)
{
	fprintf(stderr, "\r\nassertion botched: %s\r\n", s);
	(void) fflush(stderr);		/* just in case user buffered it */
	abort();
}
#else
#define	ASSERT(p)
#endif

void *
malloc(size_t nbytes)
{
	union overhead *op;
	int bucket;
	long n;
	unsigned amt;

	/*
	 * First time malloc is called, setup page size and
	 * align break pointer so all data will be page aligned.
	 */
	if (pagesz == 0) {
		pagesz = n = PAGE_SIZE;
		if (morepages(NPOOLPAGES) == 0)
			return NULL;
		op = (union overhead *)(void *)(pagepool_start);
		n = n - sizeof (*op) - ((long)op & (n - 1));
		if (n < 0)
			n += pagesz;
		if (n) {
			pagepool_start += n;
		}
		bucket = 0;
		amt = 8;
		while ((unsigned)pagesz > amt) {
			amt <<= 1;
			bucket++;
		}
		pagebucket = bucket;
	}
	/*
	 * Convert amount of memory requested into closest block size
	 * stored in hash buckets which satisfies request.
	 * Account for space used per block for accounting.
	 */
	n = pagesz - sizeof (*op) - RSLOP;
	if (nbytes <= (unsigned long)n) {
#ifndef RCHECK
		amt = 32;	/* size of first bucket */
		bucket = 2;
#else
		amt = 16;	/* size of first bucket */
		bucket = 1;
#endif
	} else {
		amt = pagesz;
		bucket = pagebucket;
	}
	n = -(sizeof (*op) + RSLOP);
	while (nbytes > (size_t)amt + n) {
		amt <<= 1;
		if (amt == 0)
			return (NULL);
		bucket++;
	}
	/*
	 * If nothing in hash bucket right now,
	 * request more memory from the system.
	 */
	if ((op = nextf[bucket]) == NULL) {
		morecore(bucket);
		if ((op = nextf[bucket]) == NULL)
			return (NULL);
	}
	/* remove from linked list */
	nextf[bucket] = op->ov_next;
	op->ov_magic = MAGIC;
	op->ov_index = bucket;
#ifdef MSTATS
	nmalloc[bucket]++;
#endif
#ifdef RCHECK
	/*
	 * Record allocated size of block and
	 * bound space with magic numbers.
	 */
	op->ov_size = (nbytes + RSLOP - 1) & ~(RSLOP - 1);
	op->ov_rmagic = RMAGIC;
	*(u_short *)((caddr_t)(op + 1) + op->ov_size) = RMAGIC;
#endif
	return ((void *)(op + 1));
}

void *
calloc(size_t num, size_t size)
{
	void *ret;

	if (size != 0 && (num * size) / size != num) {
		/* size_t overflow. */
		return (NULL);
	}

	if ((ret = malloc(num * size)) != NULL)
		memset(ret, 0, num * size);

	return (ret);
}

/*
 * Allocate more memory to the indicated bucket.
 */
static void
morecore(int bucket)
{
	char *buf;
	union overhead *op;
	int sz;				/* size of desired block */
	int amt;			/* amount to allocate */
	int nblks;			/* how many blocks we get */

	/*
	 * sbrk_size <= 0 only for big, FLUFFY, requests (about
	 * 2^30 bytes on a VAX, I think) or for a negative arg.
	 */
	sz = 1 << (bucket + 3);
#ifdef MALLOC_DEBUG
	ASSERT(sz > 0);
#else
	if (sz <= 0)
		return;
#endif
	if (sz < pagesz) {
		amt = pagesz;
		nblks = amt / sz;
	} else {
		amt = sz + pagesz;
		nblks = 1;
	}
	if (amt > pagepool_end - pagepool_start)
		if (morepages(amt/pagesz + NPOOLPAGES) == 0)
			return;

	/* non-zero offsets cause trouble */
	ASSERT(cheri_getoffset(pagepool_start) == 0);
	buf = cheri_csetbounds(pagepool_start, amt);
	pagepool_start += amt;

	/*
	 * Add new memory allocated to that on
	 * free list for this hash bucket.
	 */
	nextf[bucket] = op = cheri_csetbounds(buf, sz);;
	while (--nblks > 0) {
		op->ov_next = (union overhead *)cheri_csetbounds(buf + sz, sz);
		buf += sz;
		op = op->ov_next;
	}
}

static union overhead *
find_overhead(void * cp)
{
	union overhead *op;

	op = (union overhead *)(void *)((caddr_t)cp - sizeof (union overhead));

	if (cheri_gettag(op) && cheri_getoffset(op) == 0 &&
	    op->ov_magic == MAGIC)
	return (op);

	/*
	 * XXX: the above will fail if the users calls free or realloc
	 * with a pointer that has had CSetBounds applied to it.  We
	 * should save all allocation ranges to allow us to find the
	 * metadata.
	 */
	printf("%s: Attempting to free or realloc unallocated memory\n",
	    __func__);
	CHERI_PRINT_PTR(cp);
	return (NULL);
}

void
free(void *cp)
{
	int size;
	union overhead *op;

	if (cp == NULL)
		return;
	op = find_overhead(cp);
	if (op == NULL)
		return;
#ifdef RCHECK
	ASSERT(op->ov_rmagic == RMAGIC);
	ASSERT(*(u_short *)((caddr_t)(op + 1) + op->ov_size) == RMAGIC);
#endif
	size = op->ov_index;
	ASSERT(size < NBUCKETS);
	op->ov_next = nextf[size];	/* also clobbers ov_magic */
	nextf[size] = op;
#ifdef MSTATS
	nmalloc[size]--;
#endif
}

void *
realloc(void *cp, size_t nbytes)
{
	u_int onb;
	int i;
	union overhead *op;
	char *res;

	if (cp == NULL)
		return (malloc(nbytes));
	op = find_overhead(cp);
	if (op == NULL)
		return (NULL);
	i = op->ov_index;
	onb = 1 << (i + 3);
	if (onb < (u_int)pagesz)
		onb -= sizeof (*op) + RSLOP;
	else
		onb += pagesz - sizeof (*op) - RSLOP;

	/* avoid the copy if same size block */
	if (i > 0) {
		i = 1 << (i + 2);
		if (i < pagesz)
			i -= sizeof (*op) + RSLOP;
		else
			i += pagesz - sizeof (*op) - RSLOP;
	}
	if (nbytes <= onb && nbytes > (size_t)i) {
#ifdef RCHECK
		op->ov_size = (nbytes + RSLOP - 1) & ~(RSLOP - 1);
		*(u_short *)((caddr_t)(op + 1) + op->ov_size) = RMAGIC;
#endif
		return(cp);
	}

	if ((res = malloc(nbytes)) == NULL)
		return (NULL);
	bcopy(cp, res, (nbytes < onb) ? nbytes : onb);
	free(cp);
	return (res);
}

#ifdef MSTATS
/*
 * mstats - print out statistics about malloc
 *
 * Prints two lines of numbers, one showing the length of the free list
 * for each size category, the second showing the number of mallocs -
 * frees for each size category.
 */
mstats(char *s)
{
	int i, j;
	union overhead *p;
	int totfree = 0,
	totused = 0;

	fprintf(stderr, "Memory allocation statistics %s\nfree:\t", s);
	for (i = 0; i < NBUCKETS; i++) {
		for (j = 0, p = nextf[i]; p; p = p->ov_next, j++)
			;
		fprintf(stderr, " %d", j);
		totfree += j * (1 << (i + 3));
	}
	fprintf(stderr, "\nused:\t");
	for (i = 0; i < NBUCKETS; i++) {
		fprintf(stderr, " %d", nmalloc[i]);
		totused += nmalloc[i] * (1 << (i + 3));
	}
	fprintf(stderr, "\n\tTotal in use: %d, total free: %d\n",
	    totused, totfree);
}
#endif


static int
morepages(int n)
{
	int	fd = -1;

	if (pagepool_end - pagepool_start > pagesz) {
		caddr_t	addr = cheri_setoffset(pagepool_start,
		    roundup2(cheri_getoffset(pagepool_start), pagesz));
		if (munmap(addr, pagepool_end - addr) != 0)
			fprintf(stderr, "morepages: munmap %p",
			    addr);
	}

	if ((pagepool_start = mmap(0, n * pagesz,
			PROT_READ|PROT_WRITE,
			MAP_ANON|MAP_COPY, fd, 0)) == (caddr_t)-1) {
		printf("Cannot map anonymous memory\n");
		return 0;
	}
	pagepool_end = pagepool_start + n * pagesz;

	return n;
}
