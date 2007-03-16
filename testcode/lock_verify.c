/*
 * testcode/lock_verify.c - verifier program for lock traces, checks order.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file checks the lock traces generated by checklock.c.
 * Checks if locks are consistently locked in the same order.
 * If not, this can lead to deadlock if threads execute the different
 * ordering at the same time.
 * 
 */

#include "config.h"
#include "util/log.h"
#include "util/rbtree.h"

/* --- data structures --- */
struct lock_ref;

/** key for lock lookup */
struct order_id {
	/** the thread id that created it */
	int thr;
	/** the instance number of creation */
	int instance;
};

/** a lock */
struct order_lock {
	/** rbnode in all tree */
	rbnode_t node;
	/** lock id */
	struct order_id id;
	/** the creation file */
	char* create_file;
	/** creation line */
	int create_line;
	/** set of all locks that are smaller than this one (locked earlier) */
	rbtree_t* smaller;
	/** during depthfirstsearch, this is a linked list of the stack 
	 * of locks. points to the next lock bigger than this one. */
	struct lock_ref* dfs_next;
	/** if lock has been visited (all smaller locks have been compared to
	 * this lock), only need to compare this with all unvisited(bigger) 
	 * locks */
	int visited;
};

/** reference to a lock in a rbtree set */
struct lock_ref {
	/** rbnode, key is an order_id ptr */
	rbnode_t node;
	/** the lock referenced */
	struct order_lock* lock;
	/** why is this ref */
	char* file;
	/** line number */
	int line;
};

/** count of errors detected */
static int errors_detected = 0;

/** print program usage help */
static void
usage()
{
	printf("lock_verify <trace files>\n");
}

/** compare two order_ids */
int order_lock_cmp(const void* e1, const void* e2)
{
	struct order_id* o1 = (struct order_id*)e1;
	struct order_id* o2 = (struct order_id*)e2;
	if(o1->thr < o2->thr) return -1;
	if(o1->thr > o2->thr) return 1;
	if(o1->instance < o2->instance) return -1;
	if(o1->instance > o2->instance) return 1;
	return 0;
}

/** read header entry. 
 * @param in: file to read header of.
 * @return: False if it does not belong to the rest. */
static int 
read_header(FILE* in)
{
	time_t t;
	pid_t p;
	int thrno;
	static int have_values = 0;
	static time_t the_time;
	static pid_t the_pid;
	static int threads[256];

	if(fread(&t, sizeof(t), 1, in) != 1 ||	
		fread(&thrno, sizeof(thrno), 1, in) != 1 ||
		fread(&p, sizeof(p), 1, in) != 1) {
		fatal_exit("fread: %s", strerror(errno));
	}
	/* check these values are sorta OK */
	if(!have_values) {
		the_time = t;
		the_pid = p;
		memset(threads, 0, 256*sizeof(int));
		threads[thrno] = 1;
		have_values = 1;
		printf(" trace %d from pid %u on %s", thrno, 
			(unsigned)p, ctime(&t));
	} else {
		if(the_pid != p) {
			printf(" has pid %u, not %u. Skipped.\n",
				(unsigned)p, (unsigned)the_pid);
			return 0;
		}
		if(threads[thrno])
			fatal_exit("same threadno in two files");
		threads[thrno] = 1;
		if( abs((int)(the_time - t)) > 3600)
			fatal_exit("input files from different times: %u %u",
				(unsigned)the_time, (unsigned)t);
		printf(" trace of thread %d\n", thrno);
	}
	return 1;
}

/** max length of strings: filenames and function names. */
#define STRMAX 1024
/** read a string from file, false on error */
static int readup_str(char** str, FILE* in)
{
	char buf[STRMAX];
	int len = 0;
	int c;
	/* ends in zero */
	while( (c = fgetc(in)) != 0) {
		if(c == EOF)
			fatal_exit("eof in readstr, file too short");
		buf[len++] = c;
		if(len == STRMAX) {
			fatal_exit("string too long, bad file format");
		}
	}
	buf[len] = 0;
	*str = strdup(buf);
	return 1;
}

/** read creation entry */
static void read_create(rbtree_t* all, FILE* in)
{
	struct order_lock* o = calloc(1, sizeof(struct order_lock));
	if(!o) fatal_exit("malloc failure");
	if(fread(&o->id.thr, sizeof(int), 1, in) != 1 ||	
	   fread(&o->id.instance, sizeof(int), 1, in) != 1 ||	
	   !readup_str(&o->create_file, in) ||
	   fread(&o->create_line, sizeof(int), 1, in) != 1)
		fatal_exit("fread: %s", strerror(errno));
	o->smaller = rbtree_create(order_lock_cmp);
	o->node.key = &o->id;
	if(!rbtree_insert(all, &o->node))
		fatal_exit("lock created twice");
	if(1) printf("read create %s %d\n", o->create_file, o->create_line);
}

/** read lock entry */
static void read_lock(rbtree_t* all, FILE* in, int val)
{
	struct order_id prev_id, now_id;
	struct lock_ref* ref;
	struct order_lock* prev, *now;
	ref = (struct lock_ref*)calloc(1, sizeof(struct lock_ref));
	if(!ref) fatal_exit("malloc failure");
	prev_id.thr = val;
	if(fread(&prev_id.instance, sizeof(int), 1, in) != 1 ||	
	   fread(&now_id.thr, sizeof(int), 1, in) != 1 ||	
	   fread(&now_id.instance, sizeof(int), 1, in) != 1 ||	
	   !readup_str(&ref->file, in) ||
	   fread(&ref->line, sizeof(int), 1, in) != 1)
		fatal_exit("fread: %s", strerror(errno));
	if(1) printf("read lock %s %d\n", ref->file, ref->line);
	/* find the two locks involved */
	prev = (struct order_lock*)rbtree_search(all, &prev_id);
	now = (struct order_lock*)rbtree_search(all, &now_id);
	if(!prev || !now)
		fatal_exit("Could not find locks involved.");
	ref->lock = prev;
	ref->node.key = &prev->id;
	if(!rbtree_insert(now->smaller, &ref->node)) {
		free(ref->file);
		free(ref);
	}
}

/** read input file */
static void readinput(rbtree_t* all, char* file)
{
	FILE *in = fopen(file, "r");
	int fst;
	if(!in) {
		perror(file);
		exit(1);
	}
	printf("file %s", file);
	if(!read_header(in)) {
		fclose(in);
		return;
	}
	while(fread(&fst, sizeof(fst), 1, in) == 1) {
		if(fst == -1)
			read_create(all, in);
		else	read_lock(all, in, fst);
	}
	fclose(in);
}

/** print cycle message */
static void found_cycle(struct lock_ref* visit, int level)
{
	struct lock_ref* p;
	int i = 0;
	errors_detected++;
	printf("Found inconsistent locking order of length %d\n", level);
	printf("for lock %d %d created %s %d", 
		visit->lock->id.thr, visit->lock->id.instance,
		visit->lock->create_file, visit->lock->create_line);
	printf("sequence is:\n");
	p = visit;
	while(p) {
		struct order_lock* next = 
			p->lock->dfs_next?p->lock->dfs_next->lock:visit->lock;
		printf("[%d] is locked at line %s %d before lock %d %d\n",
			i, visit->file, visit->line, 
			next->id.thr, next->id.instance);
		printf("[%d] lock %d %d is created at %s %d\n",
			i, next->id.thr, next->id.instance,
			next->create_file, next->create_line); 
		i++;
		p = p->lock->dfs_next;
		if(p && p->lock == visit->lock)
			break;
	}
}

/** Detect cycle by comparing visited now with all (unvisited) bigger nodes. */
static int detect_cycle(struct lock_ref* visit, struct lock_ref* from)
{
	struct lock_ref* p = from;
	while(p) {
		if(p->lock == visit->lock)
			return 1;
		p = p->lock->dfs_next;
	}
	return 0;
}

/** recursive function to depth first search for cycles.
 * @param visit: the lock visited at this step.
 *	its dfs_next pointer gives the visited lock up in recursion.
 * 	same as lookfor at level 0.
 * @param level: depth of recursion. 0 is start.
 * @param from: search for matches from unvisited node upwards.
 */
static void search_cycle(struct lock_ref* visit, int level, 
	struct lock_ref* from)
{
	struct lock_ref* ref;
	/* check for cycle */
	if(detect_cycle(visit, from) && level != 0) {
		found_cycle(visit, level);
		return;
	}
	/* recurse */
	if(!visit->lock->visited)
		from = visit;
	RBTREE_FOR(ref, struct lock_ref*, visit->lock->smaller) {
		ref->lock->dfs_next = visit;
		search_cycle(ref, level+1, from);
	}
	visit->lock->visited = 1;
}

/** Check ordering of one lock */
static void check_order_lock(struct order_lock* lock)
{
	struct lock_ref start;
	if(lock->visited) return;

	start.node.key = &lock->id;
	start.lock = lock;
	start.file = lock->create_file;
	start.line = lock->create_line;

	/* depth first search to find cycle with this lock at head */
	lock->dfs_next = NULL;
	search_cycle(&start, 0, &start);
}

/** Check ordering of locks */
static void check_order(rbtree_t* all_locks)
{
	/* check each lock */
	struct order_lock* lock;
	int i=0;
	RBTREE_FOR(lock, struct order_lock*, all_locks) {
		if(1) printf("[%d/%d] Checking lock %d %d %s %d\n",
			i++, (int)all_locks->count,
			lock->id.thr, lock->id.instance, 
			lock->create_file, lock->create_line);
		check_order_lock(lock);
	}
}

/** main program to verify all traces passed */
int
main(int argc, char* argv[])
{
	rbtree_t* all_locks;
	int i;
	time_t starttime = time(NULL);
	if(argc <= 1) {
		usage();
		return 1;
	}
	log_init(NULL);
	log_ident_set("lock-verify");
	/* init */
	all_locks = rbtree_create(order_lock_cmp);
	errors_detected = 0;

	/* read the input files */
	for(i=1; i<argc; i++) {
		readinput(all_locks, argv[i]);
	}

	/* check ordering */
	check_order(all_locks);

	/* do not free a thing, OS will do it */
	printf("checked %d locks in %d seconds with %d errors.\n", 
		(int)all_locks->count, (int)(time(NULL)-starttime),
		errors_detected);
	if(errors_detected) return 1;
	return 0;
}
