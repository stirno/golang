// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"
#include "arch.h"
#include "defs.h"
#include "malloc.h"
#include "os.h"
#include "stack.h"

bool	runtime·iscgo;

static void unwindstack(G*, byte*);
static void schedule(G*);
static void acquireproc(void);
static void releaseproc(void);

typedef struct Sched Sched;

M	runtime·m0;
G	runtime·g0;	// idle goroutine for m0

static	int32	debug	= 0;

int32	runtime·gcwaiting;

// Go scheduler
//
// The go scheduler's job is to match ready-to-run goroutines (`g's)
// with waiting-for-work schedulers (`m's).  If there are ready g's
// and no waiting m's, ready() will start a new m running in a new
// OS thread, so that all ready g's can run simultaneously, up to a limit.
// For now, m's never go away.
//
// By default, Go keeps only one kernel thread (m) running user code
// at a single time; other threads may be blocked in the operating system.
// Setting the environment variable $GOMAXPROCS or calling
// runtime.GOMAXPROCS() will change the number of user threads
// allowed to execute simultaneously.  $GOMAXPROCS is thus an
// approximation of the maximum number of cores to use.
//
// Even a program that can run without deadlock in a single process
// might use more m's if given the chance.  For example, the prime
// sieve will use as many m's as there are primes (up to runtime·sched.mmax),
// allowing different stages of the pipeline to execute in parallel.
// We could revisit this choice, only kicking off new m's for blocking
// system calls, but that would limit the amount of parallel computation
// that go would try to do.
//
// In general, one could imagine all sorts of refinements to the
// scheduler, but the goal now is just to get something working on
// Linux and OS X.

struct Sched {
	Lock;

	G *gfree;	// available g's (status == Gdead)
	int32 goidgen;

	G *ghead;	// g's waiting to run
	G *gtail;
	int32 gwait;	// number of g's waiting to run
	int32 gcount;	// number of g's that are alive
	int32 grunning;	// number of g's running on cpu or in syscall

	M *mhead;	// m's waiting for work
	int32 mwait;	// number of m's waiting for work
	int32 mcount;	// number of m's that have been created

	volatile uint32 atomic;	// atomic scheduling word (see below)

	int32 predawn;		// running initialization, don't run new g's.
	int32 profilehz;	// cpu profiling rate

	Note	stopped;	// one g can set waitstop and wait here for m's to stop
};

// The atomic word in sched is an atomic uint32 that
// holds these fields.
//
//	[15 bits] mcpu		number of m's executing on cpu
//	[15 bits] mcpumax	max number of m's allowed on cpu
//	[1 bit] waitstop	some g is waiting on stopped
//	[1 bit] gwaiting	gwait != 0
//
// These fields are the information needed by entersyscall
// and exitsyscall to decide whether to coordinate with the
// scheduler.  Packing them into a single machine word lets
// them use a fast path with a single atomic read/write and
// no lock/unlock.  This greatly reduces contention in
// syscall- or cgo-heavy multithreaded programs.
//
// Except for entersyscall and exitsyscall, the manipulations
// to these fields only happen while holding the schedlock,
// so the routines holding schedlock only need to worry about
// what entersyscall and exitsyscall do, not the other routines
// (which also use the schedlock).
//
// In particular, entersyscall and exitsyscall only read mcpumax,
// waitstop, and gwaiting.  They never write them.  Thus, writes to those
// fields can be done (holding schedlock) without fear of write conflicts.
// There may still be logic conflicts: for example, the set of waitstop must
// be conditioned on mcpu >= mcpumax or else the wait may be a
// spurious sleep.  The Promela model in proc.p verifies these accesses.
enum {
	mcpuWidth = 15,
	mcpuMask = (1<<mcpuWidth) - 1,
	mcpuShift = 0,
	mcpumaxShift = mcpuShift + mcpuWidth,
	waitstopShift = mcpumaxShift + mcpuWidth,
	gwaitingShift = waitstopShift+1,

	// The max value of GOMAXPROCS is constrained
	// by the max value we can store in the bit fields
	// of the atomic word.  Reserve a few high values
	// so that we can detect accidental decrement
	// beyond zero.
	maxgomaxprocs = mcpuMask - 10,
};

#define atomic_mcpu(v)		(((v)>>mcpuShift)&mcpuMask)
#define atomic_mcpumax(v)	(((v)>>mcpumaxShift)&mcpuMask)
#define atomic_waitstop(v)	(((v)>>waitstopShift)&1)
#define atomic_gwaiting(v)	(((v)>>gwaitingShift)&1)

Sched runtime·sched;
int32 runtime·gomaxprocs;
bool runtime·singleproc;

// An m that is waiting for notewakeup(&m->havenextg).  This may
// only be accessed while the scheduler lock is held.  This is used to
// minimize the number of times we call notewakeup while the scheduler
// lock is held, since the m will normally move quickly to lock the
// scheduler itself, producing lock contention.
static M* mwakeup;

// Scheduling helpers.  Sched must be locked.
static void gput(G*);	// put/get on ghead/gtail
static G* gget(void);
static void mput(M*);	// put/get on mhead
static M* mget(G*);
static void gfput(G*);	// put/get on gfree
static G* gfget(void);
static void matchmg(void);	// match m's to g's
static void readylocked(G*);	// ready, but sched is locked
static void mnextg(M*, G*);
static void mcommoninit(M*);

void
setmcpumax(uint32 n)
{
	uint32 v, w;

	for(;;) {
		v = runtime·sched.atomic;
		w = v;
		w &= ~(mcpuMask<<mcpumaxShift);
		w |= n<<mcpumaxShift;
		if(runtime·cas(&runtime·sched.atomic, v, w))
			break;
	}
}

// The bootstrap sequence is:
//
//	call osinit
//	call schedinit
//	make & queue new G
//	call runtime·mstart
//
// The new G does:
//
//	call main·init_function
//	call initdone
//	call main·main
void
runtime·schedinit(void)
{
	int32 n;
	byte *p;

	m->nomemprof++;
	runtime·mallocinit();
	mcommoninit(m);

	runtime·goargs();
	runtime·goenvs();

	// For debugging:
	// Allocate internal symbol table representation now,
	// so that we don't need to call malloc when we crash.
	// runtime·findfunc(0);

	runtime·gomaxprocs = 1;
	p = runtime·getenv("GOMAXPROCS");
	if(p != nil && (n = runtime·atoi(p)) != 0) {
		if(n > maxgomaxprocs)
			n = maxgomaxprocs;
		runtime·gomaxprocs = n;
	}
	setmcpumax(runtime·gomaxprocs);
	runtime·singleproc = runtime·gomaxprocs == 1;
	runtime·sched.predawn = 1;

	m->nomemprof--;
}

// Lock the scheduler.
static void
schedlock(void)
{
	runtime·lock(&runtime·sched);
}

// Unlock the scheduler.
static void
schedunlock(void)
{
	M *m;

	m = mwakeup;
	mwakeup = nil;
	runtime·unlock(&runtime·sched);
	if(m != nil)
		runtime·notewakeup(&m->havenextg);
}

// Called after main·init_function; main·main will be called on return.
void
runtime·initdone(void)
{
	// Let's go.
	runtime·sched.predawn = 0;
	mstats.enablegc = 1;

	// If main·init_function started other goroutines,
	// kick off new m's to handle them, like ready
	// would have, had it not been pre-dawn.
	schedlock();
	matchmg();
	schedunlock();
}

void
runtime·goexit(void)
{
	g->status = Gmoribund;
	runtime·gosched();
}

void
runtime·goroutineheader(G *g)
{
	int8 *status;

	switch(g->status) {
	case Gidle:
		status = "idle";
		break;
	case Grunnable:
		status = "runnable";
		break;
	case Grunning:
		status = "running";
		break;
	case Gsyscall:
		status = "syscall";
		break;
	case Gwaiting:
		if(g->waitreason)
			status = g->waitreason;
		else
			status = "waiting";
		break;
	case Gmoribund:
		status = "moribund";
		break;
	default:
		status = "???";
		break;
	}
	runtime·printf("goroutine %d [%s]:\n", g->goid, status);
}

void
runtime·tracebackothers(G *me)
{
	G *g;

	for(g = runtime·allg; g != nil; g = g->alllink) {
		if(g == me || g->status == Gdead)
			continue;
		runtime·printf("\n");
		runtime·goroutineheader(g);
		runtime·traceback(g->sched.pc, g->sched.sp, 0, g);
	}
}

// Mark this g as m's idle goroutine.
// This functionality might be used in environments where programs
// are limited to a single thread, to simulate a select-driven
// network server.  It is not exposed via the standard runtime API.
void
runtime·idlegoroutine(void)
{
	if(g->idlem != nil)
		runtime·throw("g is already an idle goroutine");
	g->idlem = m;
}

static void
mcommoninit(M *m)
{
	// Add to runtime·allm so garbage collector doesn't free m
	// when it is just in a register or thread-local storage.
	m->alllink = runtime·allm;
	// runtime·Cgocalls() iterates over allm w/o schedlock,
	// so we need to publish it safely.
	runtime·atomicstorep(&runtime·allm, m);

	m->id = runtime·sched.mcount++;
	m->fastrand = 0x49f6428aUL + m->id;
	m->stackalloc = runtime·malloc(sizeof(*m->stackalloc));
	runtime·FixAlloc_Init(m->stackalloc, FixedStack, runtime·SysAlloc, nil, nil);
}

// Try to increment mcpu.  Report whether succeeded.
static bool
canaddmcpu(void)
{
	uint32 v;

	for(;;) {
		v = runtime·sched.atomic;
		if(atomic_mcpu(v) >= atomic_mcpumax(v))
			return 0;
		if(runtime·cas(&runtime·sched.atomic, v, v+(1<<mcpuShift)))
			return 1;
	}
}

// Put on `g' queue.  Sched must be locked.
static void
gput(G *g)
{
	M *m;

	// If g is wired, hand it off directly.
	if((m = g->lockedm) != nil && canaddmcpu()) {
		mnextg(m, g);
		return;
	}

	// If g is the idle goroutine for an m, hand it off.
	if(g->idlem != nil) {
		if(g->idlem->idleg != nil) {
			runtime·printf("m%d idle out of sync: g%d g%d\n",
				g->idlem->id,
				g->idlem->idleg->goid, g->goid);
			runtime·throw("runtime: double idle");
		}
		g->idlem->idleg = g;
		return;
	}

	g->schedlink = nil;
	if(runtime·sched.ghead == nil)
		runtime·sched.ghead = g;
	else
		runtime·sched.gtail->schedlink = g;
	runtime·sched.gtail = g;

	// increment gwait.
	// if it transitions to nonzero, set atomic gwaiting bit.
	if(runtime·sched.gwait++ == 0)
		runtime·xadd(&runtime·sched.atomic, 1<<gwaitingShift);
}

// Report whether gget would return something.
static bool
haveg(void)
{
	return runtime·sched.ghead != nil || m->idleg != nil;
}

// Get from `g' queue.  Sched must be locked.
static G*
gget(void)
{
	G *g;

	g = runtime·sched.ghead;
	if(g){
		runtime·sched.ghead = g->schedlink;
		if(runtime·sched.ghead == nil)
			runtime·sched.gtail = nil;
		// decrement gwait.
		// if it transitions to zero, clear atomic gwaiting bit.
		if(--runtime·sched.gwait == 0)
			runtime·xadd(&runtime·sched.atomic, -1<<gwaitingShift);
	} else if(m->idleg != nil) {
		g = m->idleg;
		m->idleg = nil;
	}
	return g;
}

// Put on `m' list.  Sched must be locked.
static void
mput(M *m)
{
	m->schedlink = runtime·sched.mhead;
	runtime·sched.mhead = m;
	runtime·sched.mwait++;
}

// Get an `m' to run `g'.  Sched must be locked.
static M*
mget(G *g)
{
	M *m;

	// if g has its own m, use it.
	if((m = g->lockedm) != nil)
		return m;

	// otherwise use general m pool.
	if((m = runtime·sched.mhead) != nil){
		runtime·sched.mhead = m->schedlink;
		runtime·sched.mwait--;
	}
	return m;
}

// Mark g ready to run.
void
runtime·ready(G *g)
{
	schedlock();
	readylocked(g);
	schedunlock();
}

// Mark g ready to run.  Sched is already locked.
// G might be running already and about to stop.
// The sched lock protects g->status from changing underfoot.
static void
readylocked(G *g)
{
	if(g->m){
		// Running on another machine.
		// Ready it when it stops.
		g->readyonstop = 1;
		return;
	}

	// Mark runnable.
	if(g->status == Grunnable || g->status == Grunning) {
		runtime·printf("goroutine %d has status %d\n", g->goid, g->status);
		runtime·throw("bad g->status in ready");
	}
	g->status = Grunnable;

	gput(g);
	if(!runtime·sched.predawn)
		matchmg();
}

static void
nop(void)
{
}

// Same as readylocked but a different symbol so that
// debuggers can set a breakpoint here and catch all
// new goroutines.
static void
newprocreadylocked(G *g)
{
	nop();	// avoid inlining in 6l
	readylocked(g);
}

// Pass g to m for running.
// Caller has already incremented mcpu.
static void
mnextg(M *m, G *g)
{
	runtime·sched.grunning++;
	m->nextg = g;
	if(m->waitnextg) {
		m->waitnextg = 0;
		if(mwakeup != nil)
			runtime·notewakeup(&mwakeup->havenextg);
		mwakeup = m;
	}
}

// Get the next goroutine that m should run.
// Sched must be locked on entry, is unlocked on exit.
// Makes sure that at most $GOMAXPROCS g's are
// running on cpus (not in system calls) at any given time.
static G*
nextgandunlock(void)
{
	G *gp;
	uint32 v;

	if(atomic_mcpu(runtime·sched.atomic) >= maxgomaxprocs)
		runtime·throw("negative mcpu");

	// If there is a g waiting as m->nextg, the mcpu++
	// happened before it was passed to mnextg.
	if(m->nextg != nil) {
		gp = m->nextg;
		m->nextg = nil;
		schedunlock();
		return gp;
	}

	if(m->lockedg != nil) {
		// We can only run one g, and it's not available.
		// Make sure some other cpu is running to handle
		// the ordinary run queue.
		if(runtime·sched.gwait != 0) {
			matchmg();
			// m->lockedg might have been on the queue.
			if(m->nextg != nil) {
				gp = m->nextg;
				m->nextg = nil;
				schedunlock();
				return gp;
			}
		}
	} else {
		// Look for work on global queue.
		while(haveg() && canaddmcpu()) {
			gp = gget();
			if(gp == nil)
				runtime·throw("gget inconsistency");

			if(gp->lockedm) {
				mnextg(gp->lockedm, gp);
				continue;
			}
			runtime·sched.grunning++;
			schedunlock();
			return gp;
		}

		// The while loop ended either because the g queue is empty
		// or because we have maxed out our m procs running go
		// code (mcpu >= mcpumax).  We need to check that
		// concurrent actions by entersyscall/exitsyscall cannot
		// invalidate the decision to end the loop.
		//
		// We hold the sched lock, so no one else is manipulating the
		// g queue or changing mcpumax.  Entersyscall can decrement
		// mcpu, but if does so when there is something on the g queue,
		// the gwait bit will be set, so entersyscall will take the slow path
		// and use the sched lock.  So it cannot invalidate our decision.
		//
		// Wait on global m queue.
		mput(m);
	}

	v = runtime·atomicload(&runtime·sched.atomic);
	if(runtime·sched.grunning == 0)
		runtime·throw("all goroutines are asleep - deadlock!");
	m->nextg = nil;
	m->waitnextg = 1;
	runtime·noteclear(&m->havenextg);

	// Stoptheworld is waiting for all but its cpu to go to stop.
	// Entersyscall might have decremented mcpu too, but if so
	// it will see the waitstop and take the slow path.
	// Exitsyscall never increments mcpu beyond mcpumax.
	if(atomic_waitstop(v) && atomic_mcpu(v) <= atomic_mcpumax(v)) {
		// set waitstop = 0 (known to be 1)
		runtime·xadd(&runtime·sched.atomic, -1<<waitstopShift);
		runtime·notewakeup(&runtime·sched.stopped);
	}
	schedunlock();

	runtime·notesleep(&m->havenextg);
	if((gp = m->nextg) == nil)
		runtime·throw("bad m->nextg in nextgoroutine");
	m->nextg = nil;
	return gp;
}

void
runtime·stoptheworld(void)
{
	uint32 v;

	schedlock();
	runtime·gcwaiting = 1;

	setmcpumax(1);

	// while mcpu > 1
	for(;;) {
		v = runtime·sched.atomic;
		if(atomic_mcpu(v) <= 1)
			break;

		// It would be unsafe for multiple threads to be using
		// the stopped note at once, but there is only
		// ever one thread doing garbage collection.
		runtime·noteclear(&runtime·sched.stopped);
		if(atomic_waitstop(v))
			runtime·throw("invalid waitstop");

		// atomic { waitstop = 1 }, predicated on mcpu <= 1 check above
		// still being true.
		if(!runtime·cas(&runtime·sched.atomic, v, v+(1<<waitstopShift)))
			continue;

		schedunlock();
		runtime·notesleep(&runtime·sched.stopped);
		schedlock();
	}
	runtime·singleproc = runtime·gomaxprocs == 1;
	schedunlock();
}

// TODO(rsc): Remove. This is only temporary,
// for the mark and sweep collector.
void
runtime·starttheworld(void)
{
	schedlock();
	runtime·gcwaiting = 0;
	setmcpumax(runtime·gomaxprocs);
	matchmg();
	schedunlock();
}

// Called to start an M.
void
runtime·mstart(void)
{
	if(g != m->g0)
		runtime·throw("bad runtime·mstart");
	if(m->mcache == nil)
		m->mcache = runtime·allocmcache();

	// Record top of stack for use by mcall.
	// Once we call schedule we're never coming back,
	// so other calls can reuse this stack space.
	runtime·gosave(&m->g0->sched);
	m->g0->sched.pc = (void*)-1;  // make sure it is never used

	runtime·minit();
	schedule(nil);
}

// When running with cgo, we call libcgo_thread_start
// to start threads for us so that we can play nicely with
// foreign code.
void (*libcgo_thread_start)(void*);

typedef struct CgoThreadStart CgoThreadStart;
struct CgoThreadStart
{
	M *m;
	G *g;
	void (*fn)(void);
};

// Kick off new m's as needed (up to mcpumax).
// There are already `other' other cpus that will
// start looking for goroutines shortly.
// Sched is locked.
static void
matchmg(void)
{
	G *g;

	if(m->mallocing || m->gcing)
		return;

	while(haveg() && canaddmcpu()) {
		g = gget();
		if(g == nil)
			runtime·throw("gget inconsistency");

		// Find the m that will run g.
		M *m;
		if((m = mget(g)) == nil){
			m = runtime·malloc(sizeof(M));
			mcommoninit(m);

			if(runtime·iscgo) {
				CgoThreadStart ts;

				if(libcgo_thread_start == nil)
					runtime·throw("libcgo_thread_start missing");
				// pthread_create will make us a stack.
				m->g0 = runtime·malg(-1);
				ts.m = m;
				ts.g = m->g0;
				ts.fn = runtime·mstart;
				runtime·asmcgocall(libcgo_thread_start, &ts);
			} else {
				if(Windows)
					// windows will layout sched stack on os stack
					m->g0 = runtime·malg(-1);
				else
					m->g0 = runtime·malg(8192);
				runtime·newosproc(m, m->g0, m->g0->stackbase, runtime·mstart);
			}
		}
		mnextg(m, g);
	}
}

// One round of scheduler: find a goroutine and run it.
// The argument is the goroutine that was running before
// schedule was called, or nil if this is the first call.
// Never returns.
static void
schedule(G *gp)
{
	int32 hz;
	uint32 v;

	schedlock();
	if(gp != nil) {
		if(runtime·sched.predawn)
			runtime·throw("init rescheduling");

		// Just finished running gp.
		gp->m = nil;
		runtime·sched.grunning--;

		// atomic { mcpu-- }
		v = runtime·xadd(&runtime·sched.atomic, -1<<mcpuShift);
		if(atomic_mcpu(v) > maxgomaxprocs)
			runtime·throw("negative mcpu in scheduler");

		switch(gp->status){
		case Grunnable:
		case Gdead:
			// Shouldn't have been running!
			runtime·throw("bad gp->status in sched");
		case Grunning:
			gp->status = Grunnable;
			gput(gp);
			break;
		case Gmoribund:
			gp->status = Gdead;
			if(gp->lockedm) {
				gp->lockedm = nil;
				m->lockedg = nil;
			}
			gp->idlem = nil;
			unwindstack(gp, nil);
			gfput(gp);
			if(--runtime·sched.gcount == 0)
				runtime·exit(0);
			break;
		}
		if(gp->readyonstop){
			gp->readyonstop = 0;
			readylocked(gp);
		}
	}

	// Find (or wait for) g to run.  Unlocks runtime·sched.
	gp = nextgandunlock();
	gp->readyonstop = 0;
	gp->status = Grunning;
	m->curg = gp;
	gp->m = m;

	// Check whether the profiler needs to be turned on or off.
	hz = runtime·sched.profilehz;
	if(m->profilehz != hz)
		runtime·resetcpuprofiler(hz);

	if(gp->sched.pc == (byte*)runtime·goexit) {	// kickoff
		runtime·gogocall(&gp->sched, (void(*)(void))gp->entry);
	}
	runtime·gogo(&gp->sched, 0);
}

// Enter scheduler.  If g->status is Grunning,
// re-queues g and runs everyone else who is waiting
// before running g again.  If g->status is Gmoribund,
// kills off g.
// Cannot split stack because it is called from exitsyscall.
// See comment below.
#pragma textflag 7
void
runtime·gosched(void)
{
	if(m->locks != 0)
		runtime·throw("gosched holding locks");
	if(g == m->g0)
		runtime·throw("gosched of g0");
	runtime·mcall(schedule);
}

// The goroutine g is about to enter a system call.
// Record that it's not using the cpu anymore.
// This is called only from the go syscall library and cgocall,
// not from the low-level system calls used by the runtime.
//
// Entersyscall cannot split the stack: the runtime·gosave must
// make g->sched refer to the caller's stack segment, because
// entersyscall is going to return immediately after.
// It's okay to call matchmg and notewakeup even after
// decrementing mcpu, because we haven't released the
// sched lock yet, so the garbage collector cannot be running.
#pragma textflag 7
void
runtime·entersyscall(void)
{
	uint32 v;

	if(runtime·sched.predawn)
		return;

	// Leave SP around for gc and traceback.
	runtime·gosave(&g->sched);
	g->gcsp = g->sched.sp;
	g->gcstack = g->stackbase;
	g->gcguard = g->stackguard;
	g->status = Gsyscall;
	if(g->gcsp < g->gcguard-StackGuard || g->gcstack < g->gcsp) {
		// runtime·printf("entersyscall inconsistent %p [%p,%p]\n",
		//	g->gcsp, g->gcguard-StackGuard, g->gcstack);
		runtime·throw("entersyscall");
	}

	// Fast path.
	// The slow path inside the schedlock/schedunlock will get
	// through without stopping if it does:
	//	mcpu--
	//	gwait not true
	//	waitstop && mcpu <= mcpumax not true
	// If we can do the same with a single atomic add,
	// then we can skip the locks.
	v = runtime·xadd(&runtime·sched.atomic, -1<<mcpuShift);
	if(!atomic_gwaiting(v) && (!atomic_waitstop(v) || atomic_mcpu(v) > atomic_mcpumax(v)))
		return;

	schedlock();
	v = runtime·atomicload(&runtime·sched.atomic);
	if(atomic_gwaiting(v)) {
		matchmg();
		v = runtime·atomicload(&runtime·sched.atomic);
	}
	if(atomic_waitstop(v) && atomic_mcpu(v) <= atomic_mcpumax(v)) {
		runtime·xadd(&runtime·sched.atomic, -1<<waitstopShift);
		runtime·notewakeup(&runtime·sched.stopped);
	}

	// Re-save sched in case one of the calls
	// (notewakeup, matchmg) triggered something using it.
	runtime·gosave(&g->sched);

	schedunlock();
}

// The goroutine g exited its system call.
// Arrange for it to run on a cpu again.
// This is called only from the go syscall library, not
// from the low-level system calls used by the runtime.
void
runtime·exitsyscall(void)
{
	uint32 v;

	if(runtime·sched.predawn)
		return;

	// Fast path.
	// If we can do the mcpu++ bookkeeping and
	// find that we still have mcpu <= mcpumax, then we can
	// start executing Go code immediately, without having to
	// schedlock/schedunlock.
	v = runtime·xadd(&runtime·sched.atomic, (1<<mcpuShift));
	if(m->profilehz == runtime·sched.profilehz && atomic_mcpu(v) <= atomic_mcpumax(v)) {
		// There's a cpu for us, so we can run.
		g->status = Grunning;
		// Garbage collector isn't running (since we are),
		// so okay to clear gcstack.
		g->gcstack = nil;
		return;
	}

	// Tell scheduler to put g back on the run queue:
	// mostly equivalent to g->status = Grunning,
	// but keeps the garbage collector from thinking
	// that g is running right now, which it's not.
	g->readyonstop = 1;

	// All the cpus are taken.
	// The scheduler will ready g and put this m to sleep.
	// When the scheduler takes g away from m,
	// it will undo the runtime·sched.mcpu++ above.
	runtime·gosched();

	// Gosched returned, so we're allowed to run now.
	// Delete the gcstack information that we left for
	// the garbage collector during the system call.
	// Must wait until now because until gosched returns
	// we don't know for sure that the garbage collector
	// is not running.
	g->gcstack = nil;
}

void
runtime·oldstack(void)
{
	Stktop *top, old;
	uint32 argsize;
	byte *sp;
	G *g1;
	int32 goid;

//printf("oldstack m->cret=%p\n", m->cret);

	g1 = m->curg;
	top = (Stktop*)g1->stackbase;
	sp = (byte*)top;
	old = *top;
	argsize = old.argsize;
	if(argsize > 0) {
		sp -= argsize;
		runtime·memmove(top->argp, sp, argsize);
	}
	goid = old.gobuf.g->goid;	// fault if g is bad, before gogo
	USED(goid);

	if(old.free != 0)
		runtime·stackfree(g1->stackguard - StackGuard, old.free);
	g1->stackbase = old.stackbase;
	g1->stackguard = old.stackguard;

	runtime·gogo(&old.gobuf, m->cret);
}

void
runtime·newstack(void)
{
	int32 framesize, argsize;
	Stktop *top;
	byte *stk, *sp;
	G *g1;
	Gobuf label;
	bool reflectcall;
	uintptr free;

	framesize = m->moreframesize;
	argsize = m->moreargsize;
	g1 = m->curg;

	if(m->morebuf.sp < g1->stackguard - StackGuard) {
		runtime·printf("runtime: split stack overflow: %p < %p\n", m->morebuf.sp, g1->stackguard - StackGuard);
		runtime·throw("runtime: split stack overflow");
	}
	if(argsize % sizeof(uintptr) != 0) {
		runtime·printf("runtime: stack split with misaligned argsize %d\n", argsize);
		runtime·throw("runtime: stack split argsize");
	}

	reflectcall = framesize==1;
	if(reflectcall)
		framesize = 0;

	if(reflectcall && m->morebuf.sp - sizeof(Stktop) - argsize - 32 > g1->stackguard) {
		// special case: called from reflect.call (framesize==1)
		// to call code with an arbitrary argument size,
		// and we have enough space on the current stack.
		// the new Stktop* is necessary to unwind, but
		// we don't need to create a new segment.
		top = (Stktop*)(m->morebuf.sp - sizeof(*top));
		stk = g1->stackguard - StackGuard;
		free = 0;
	} else {
		// allocate new segment.
		framesize += argsize;
		framesize += StackExtra;	// room for more functions, Stktop.
		if(framesize < StackMin)
			framesize = StackMin;
		framesize += StackSystem;
		stk = runtime·stackalloc(framesize);
		top = (Stktop*)(stk+framesize-sizeof(*top));
		free = framesize;
	}

//runtime·printf("newstack framesize=%d argsize=%d morepc=%p moreargp=%p gobuf=%p, %p top=%p old=%p\n",
//framesize, argsize, m->morepc, m->moreargp, m->morebuf.pc, m->morebuf.sp, top, g1->stackbase);

	top->stackbase = g1->stackbase;
	top->stackguard = g1->stackguard;
	top->gobuf = m->morebuf;
	top->argp = m->moreargp;
	top->argsize = argsize;
	top->free = free;

	// copy flag from panic
	top->panic = g1->ispanic;
	g1->ispanic = false;

	g1->stackbase = (byte*)top;
	g1->stackguard = stk + StackGuard;

	sp = (byte*)top;
	if(argsize > 0) {
		sp -= argsize;
		runtime·memmove(sp, m->moreargp, argsize);
	}
	if(thechar == '5') {
		// caller would have saved its LR below args.
		sp -= sizeof(void*);
		*(void**)sp = nil;
	}

	// Continue as if lessstack had just called m->morepc
	// (the PC that decided to grow the stack).
	label.sp = sp;
	label.pc = (byte*)runtime·lessstack;
	label.g = m->curg;
	runtime·gogocall(&label, m->morepc);

	*(int32*)345 = 123;	// never return
}

static void
mstackalloc(G *gp)
{
	gp->param = runtime·stackalloc((uintptr)gp->param);
	runtime·gogo(&gp->sched, 0);
}

G*
runtime·malg(int32 stacksize)
{
	G *newg;
	byte *stk;

	newg = runtime·malloc(sizeof(G));
	if(stacksize >= 0) {
		if(g == m->g0) {
			// running on scheduler stack already.
			stk = runtime·stackalloc(StackSystem + stacksize);
		} else {
			// have to call stackalloc on scheduler stack.
			g->param = (void*)(StackSystem + stacksize);
			runtime·mcall(mstackalloc);
			stk = g->param;
			g->param = nil;
		}
		newg->stack0 = stk;
		newg->stackguard = stk + StackGuard;
		newg->stackbase = stk + StackSystem + stacksize - sizeof(Stktop);
		runtime·memclr(newg->stackbase, sizeof(Stktop));
	}
	return newg;
}

/*
 * Newproc and deferproc need to be textflag 7
 * (no possible stack split when nearing overflow)
 * because they assume that the arguments to fn
 * are available sequentially beginning at &arg0.
 * If a stack split happened, only the one word
 * arg0 would be copied.  It's okay if any functions
 * they call split the stack below the newproc frame.
 */
#pragma textflag 7
void
runtime·newproc(int32 siz, byte* fn, ...)
{
	byte *argp;

	if(thechar == '5')
		argp = (byte*)(&fn+2);  // skip caller's saved LR
	else
		argp = (byte*)(&fn+1);
	runtime·newproc1(fn, argp, siz, 0, runtime·getcallerpc(&siz));
}

G*
runtime·newproc1(byte *fn, byte *argp, int32 narg, int32 nret, void *callerpc)
{
	byte *sp;
	G *newg;
	int32 siz;

//printf("newproc1 %p %p narg=%d nret=%d\n", fn, argp, narg, nret);
	siz = narg + nret;
	siz = (siz+7) & ~7;
	
	// We could instead create a secondary stack frame
	// and make it look like goexit was on the original but
	// the call to the actual goroutine function was split.
	// Not worth it: this is almost always an error.
	if(siz > StackMin - 1024)
		runtime·throw("runtime.newproc: function arguments too large for new goroutine");

	schedlock();

	if((newg = gfget()) != nil){
		if(newg->stackguard - StackGuard != newg->stack0)
			runtime·throw("invalid stack in newg");
	} else {
		newg = runtime·malg(StackMin);
		if(runtime·lastg == nil)
			runtime·allg = newg;
		else
			runtime·lastg->alllink = newg;
		runtime·lastg = newg;
	}
	newg->status = Gwaiting;
	newg->waitreason = "new goroutine";

	sp = newg->stackbase;
	sp -= siz;
	runtime·memmove(sp, argp, narg);
	if(thechar == '5') {
		// caller's LR
		sp -= sizeof(void*);
		*(void**)sp = nil;
	}

	newg->sched.sp = sp;
	newg->sched.pc = (byte*)runtime·goexit;
	newg->sched.g = newg;
	newg->entry = fn;
	newg->gopc = (uintptr)callerpc;

	runtime·sched.gcount++;
	runtime·sched.goidgen++;
	newg->goid = runtime·sched.goidgen;

	newprocreadylocked(newg);
	schedunlock();

	return newg;
//printf(" goid=%d\n", newg->goid);
}

#pragma textflag 7
uintptr
runtime·deferproc(int32 siz, byte* fn, ...)
{
	Defer *d;

	d = runtime·malloc(sizeof(*d) + siz - sizeof(d->args));
	d->fn = fn;
	d->siz = siz;
	d->pc = runtime·getcallerpc(&siz);
	if(thechar == '5')
		d->argp = (byte*)(&fn+2);  // skip caller's saved link register
	else
		d->argp = (byte*)(&fn+1);
	runtime·memmove(d->args, d->argp, d->siz);

	d->link = g->defer;
	g->defer = d;

	// deferproc returns 0 normally.
	// a deferred func that stops a panic
	// makes the deferproc return 1.
	// the code the compiler generates always
	// checks the return value and jumps to the
	// end of the function if deferproc returns != 0.
	return 0;
}

#pragma textflag 7
void
runtime·deferreturn(uintptr arg0)
{
	Defer *d;
	byte *argp, *fn;

	d = g->defer;
	if(d == nil)
		return;
	argp = (byte*)&arg0;
	if(d->argp != argp)
		return;
	runtime·memmove(argp, d->args, d->siz);
	g->defer = d->link;
	fn = d->fn;
	if(!d->nofree)
		runtime·free(d);
	runtime·jmpdefer(fn, argp);
}

static void
rundefer(void)
{
	Defer *d;

	while((d = g->defer) != nil) {
		g->defer = d->link;
		reflect·call(d->fn, d->args, d->siz);
		if(!d->nofree)
			runtime·free(d);
	}
}

// Free stack frames until we hit the last one
// or until we find the one that contains the argp.
static void
unwindstack(G *gp, byte *sp)
{
	Stktop *top;
	byte *stk;

	// Must be called from a different goroutine, usually m->g0.
	if(g == gp)
		runtime·throw("unwindstack on self");

	while((top = (Stktop*)gp->stackbase) != nil && top->stackbase != nil) {
		stk = gp->stackguard - StackGuard;
		if(stk <= sp && sp < gp->stackbase)
			break;
		gp->stackbase = top->stackbase;
		gp->stackguard = top->stackguard;
		if(top->free != 0)
			runtime·stackfree(stk, top->free);
	}

	if(sp != nil && (sp < gp->stackguard - StackGuard || gp->stackbase < sp)) {
		runtime·printf("recover: %p not in [%p, %p]\n", sp, gp->stackguard - StackGuard, gp->stackbase);
		runtime·throw("bad unwindstack");
	}
}

static void
printpanics(Panic *p)
{
	if(p->link) {
		printpanics(p->link);
		runtime·printf("\t");
	}
	runtime·printf("panic: ");
	runtime·printany(p->arg);
	if(p->recovered)
		runtime·printf(" [recovered]");
	runtime·printf("\n");
}

static void recovery(G*);

void
runtime·panic(Eface e)
{
	Defer *d;
	Panic *p;

	p = runtime·mal(sizeof *p);
	p->arg = e;
	p->link = g->panic;
	p->stackbase = g->stackbase;
	g->panic = p;

	for(;;) {
		d = g->defer;
		if(d == nil)
			break;
		// take defer off list in case of recursive panic
		g->defer = d->link;
		g->ispanic = true;	// rock for newstack, where reflect.call ends up
		reflect·call(d->fn, d->args, d->siz);
		if(p->recovered) {
			g->panic = p->link;
			if(g->panic == nil)	// must be done with signal
				g->sig = 0;
			runtime·free(p);
			// put recovering defer back on list
			// for scheduler to find.
			d->link = g->defer;
			g->defer = d;
			runtime·mcall(recovery);
			runtime·throw("recovery failed"); // mcall should not return
		}
		if(!d->nofree)
			runtime·free(d);
	}

	// ran out of deferred calls - old-school panic now
	runtime·startpanic();
	printpanics(g->panic);
	runtime·dopanic(0);
}

static void
recovery(G *gp)
{
	Defer *d;

	// Rewind gp's stack; we're running on m->g0's stack.
	d = gp->defer;
	gp->defer = d->link;

	// Unwind to the stack frame with d's arguments in it.
	unwindstack(gp, d->argp);

	// Make the deferproc for this d return again,
	// this time returning 1.  The calling function will
	// jump to the standard return epilogue.
	// The -2*sizeof(uintptr) makes up for the
	// two extra words that are on the stack at
	// each call to deferproc.
	// (The pc we're returning to does pop pop
	// before it tests the return value.)
	// On the arm there are 2 saved LRs mixed in too.
	if(thechar == '5')
		gp->sched.sp = (byte*)d->argp - 4*sizeof(uintptr);
	else
		gp->sched.sp = (byte*)d->argp - 2*sizeof(uintptr);
	gp->sched.pc = d->pc;
	if(!d->nofree)
		runtime·free(d);
	runtime·gogo(&gp->sched, 1);
}

#pragma textflag 7	/* no split, or else g->stackguard is not the stack for fp */
void
runtime·recover(byte *argp, Eface ret)
{
	Stktop *top, *oldtop;
	Panic *p;

	// Must be a panic going on.
	if((p = g->panic) == nil || p->recovered)
		goto nomatch;

	// Frame must be at the top of the stack segment,
	// because each deferred call starts a new stack
	// segment as a side effect of using reflect.call.
	// (There has to be some way to remember the
	// variable argument frame size, and the segment
	// code already takes care of that for us, so we
	// reuse it.)
	//
	// As usual closures complicate things: the fp that
	// the closure implementation function claims to have
	// is where the explicit arguments start, after the
	// implicit pointer arguments and PC slot.
	// If we're on the first new segment for a closure,
	// then fp == top - top->args is correct, but if
	// the closure has its own big argument frame and
	// allocated a second segment (see below),
	// the fp is slightly above top - top->args.
	// That condition can't happen normally though
	// (stack pointers go down, not up), so we can accept
	// any fp between top and top - top->args as
	// indicating the top of the segment.
	top = (Stktop*)g->stackbase;
	if(argp < (byte*)top - top->argsize || (byte*)top < argp)
		goto nomatch;

	// The deferred call makes a new segment big enough
	// for the argument frame but not necessarily big
	// enough for the function's local frame (size unknown
	// at the time of the call), so the function might have
	// made its own segment immediately.  If that's the
	// case, back top up to the older one, the one that
	// reflect.call would have made for the panic.
	//
	// The fp comparison here checks that the argument
	// frame that was copied during the split (the top->args
	// bytes above top->fp) abuts the old top of stack.
	// This is a correct test for both closure and non-closure code.
	oldtop = (Stktop*)top->stackbase;
	if(oldtop != nil && top->argp == (byte*)oldtop - top->argsize)
		top = oldtop;

	// Now we have the segment that was created to
	// run this call.  It must have been marked as a panic segment.
	if(!top->panic)
		goto nomatch;

	// Okay, this is the top frame of a deferred call
	// in response to a panic.  It can see the panic argument.
	p->recovered = 1;
	ret = p->arg;
	FLUSH(&ret);
	return;

nomatch:
	ret.type = nil;
	ret.data = nil;
	FLUSH(&ret);
}


// Put on gfree list.  Sched must be locked.
static void
gfput(G *g)
{
	if(g->stackguard - StackGuard != g->stack0)
		runtime·throw("invalid stack in gfput");
	g->schedlink = runtime·sched.gfree;
	runtime·sched.gfree = g;
}

// Get from gfree list.  Sched must be locked.
static G*
gfget(void)
{
	G *g;

	g = runtime·sched.gfree;
	if(g)
		runtime·sched.gfree = g->schedlink;
	return g;
}

void
runtime·Breakpoint(void)
{
	runtime·breakpoint();
}

void
runtime·Goexit(void)
{
	rundefer();
	runtime·goexit();
}

void
runtime·Gosched(void)
{
	runtime·gosched();
}

void
runtime·LockOSThread(void)
{
	if(runtime·sched.predawn)
		runtime·throw("cannot wire during init");
	m->lockedg = g;
	g->lockedm = m;
}

// delete when scheduler is stronger
int32
runtime·gomaxprocsfunc(int32 n)
{
	int32 ret;
	uint32 v;

	schedlock();
	ret = runtime·gomaxprocs;
	if(n <= 0)
		n = ret;
	if(n > maxgomaxprocs)
		n = maxgomaxprocs;
	runtime·gomaxprocs = n;
	if(runtime·gomaxprocs > 1)
		runtime·singleproc = false;
 	if(runtime·gcwaiting != 0) {
 		if(atomic_mcpumax(runtime·sched.atomic) != 1)
 			runtime·throw("invalid mcpumax during gc");
		schedunlock();
		return ret;
	}

	setmcpumax(n);

	// If there are now fewer allowed procs
	// than procs running, stop.
	v = runtime·atomicload(&runtime·sched.atomic);
	if(atomic_mcpu(v) > n) {
		schedunlock();
		runtime·gosched();
		return ret;
	}
	// handle more procs
	matchmg();
	schedunlock();
	return ret;
}

void
runtime·UnlockOSThread(void)
{
	m->lockedg = nil;
	g->lockedm = nil;
}

bool
runtime·lockedOSThread(void)
{
	return g->lockedm != nil && m->lockedg != nil;
}

// for testing of wire, unwire
void
runtime·mid(uint32 ret)
{
	ret = m->id;
	FLUSH(&ret);
}

void
runtime·Goroutines(int32 ret)
{
	ret = runtime·sched.gcount;
	FLUSH(&ret);
}

int32
runtime·mcount(void)
{
	return runtime·sched.mcount;
}

void
runtime·badmcall(void)  // called from assembly
{
	runtime·throw("runtime: mcall called on m->g0 stack");
}

void
runtime·badmcall2(void)  // called from assembly
{
	runtime·throw("runtime: mcall function returned");
}

static struct {
	Lock;
	void (*fn)(uintptr*, int32);
	int32 hz;
	uintptr pcbuf[100];
} prof;

void
runtime·sigprof(uint8 *pc, uint8 *sp, uint8 *lr, G *gp)
{
	int32 n;

	if(prof.fn == nil || prof.hz == 0)
		return;

	runtime·lock(&prof);
	if(prof.fn == nil) {
		runtime·unlock(&prof);
		return;
	}
	n = runtime·gentraceback(pc, sp, lr, gp, 0, prof.pcbuf, nelem(prof.pcbuf));
	if(n > 0)
		prof.fn(prof.pcbuf, n);
	runtime·unlock(&prof);
}

void
runtime·setcpuprofilerate(void (*fn)(uintptr*, int32), int32 hz)
{
	// Force sane arguments.
	if(hz < 0)
		hz = 0;
	if(hz == 0)
		fn = nil;
	if(fn == nil)
		hz = 0;

	// Stop profiler on this cpu so that it is safe to lock prof.
	// if a profiling signal came in while we had prof locked,
	// it would deadlock.
	runtime·resetcpuprofiler(0);

	runtime·lock(&prof);
	prof.fn = fn;
	prof.hz = hz;
	runtime·unlock(&prof);
	runtime·lock(&runtime·sched);
	runtime·sched.profilehz = hz;
	runtime·unlock(&runtime·sched);

	if(hz != 0)
		runtime·resetcpuprofiler(hz);
}

void (*libcgo_setenv)(byte**);

void
os·setenv_c(String k, String v)
{
	byte *arg[2];

	if(libcgo_setenv == nil)
		return;

	arg[0] = runtime·malloc(k.len + 1);
	runtime·memmove(arg[0], k.str, k.len);
	arg[0][k.len] = 0;

	arg[1] = runtime·malloc(v.len + 1);
	runtime·memmove(arg[1], v.str, v.len);
	arg[1][v.len] = 0;

	runtime·asmcgocall((void*)libcgo_setenv, arg);
	runtime·free(arg[0]);
	runtime·free(arg[1]);
}
