/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TRACE_RECURSION_H
#define _LINUX_TRACE_RECURSION_H

#include <linux/interrupt.h>
#include <linux/sched.h>

#ifdef CONFIG_TRACING

/* Only current can touch trace_recursion */

/*
 * For function tracing recursion:
 *  The order of these bits are important.
 *
 *  When function tracing occurs, the following steps are made:
 *   If arch does not support a ftrace feature:
 *    call internal function (uses INTERNAL bits) which calls...
 *   If callback is registered to the "global" list, the list
 *    function is called and recursion checks the GLOBAL bits.
 *    then this function calls...
 *   The function callback, which can use the FTRACE bits to
 *    check for recursion.
 */
enum {
	/* Function recursion bits */
	TRACE_FTRACE_BIT,
	TRACE_FTRACE_NMI_BIT,
	TRACE_FTRACE_IRQ_BIT,
	TRACE_FTRACE_SIRQ_BIT,
	TRACE_FTRACE_TRANSITION_BIT,

	/* Internal use recursion bits */
	TRACE_INTERNAL_BIT,
	TRACE_INTERNAL_NMI_BIT,
	TRACE_INTERNAL_IRQ_BIT,
	TRACE_INTERNAL_SIRQ_BIT,
	TRACE_INTERNAL_TRANSITION_BIT,

	TRACE_BRANCH_BIT,
/*
 * Abuse of the trace_recursion.
 * As we need a way to maintain state if we are tracing the function
 * graph in irq because we want to trace a particular function that
 * was called in irq context but we have irq tracing off. Since this
 * can only be modified by current, we can reuse trace_recursion.
 */
	TRACE_IRQ_BIT,

	/* Set if the function is in the set_graph_function file */
	TRACE_GRAPH_BIT,

	/*
	 * In the very unlikely case that an interrupt came in
	 * at a start of graph tracing, and we want to trace
	 * the function in that interrupt, the depth can be greater
	 * than zero, because of the preempted start of a previous
	 * trace. In an even more unlikely case, depth could be 2
	 * if a softirq interrupted the start of graph tracing,
	 * followed by an interrupt preempting a start of graph
	 * tracing in the softirq, and depth can even be 3
	 * if an NMI came in at the start of an interrupt function
	 * that preempted a softirq start of a function that
	 * preempted normal context!!!! Luckily, it can't be
	 * greater than 3, so the next two bits are a mask
	 * of what the depth is when we set TRACE_GRAPH_BIT
	 */

	TRACE_GRAPH_DEPTH_START_BIT,
	TRACE_GRAPH_DEPTH_END_BIT,

	/*
	 * To implement set_graph_notrace, if this bit is set, we ignore
	 * function graph tracing of called functions, until the return
	 * function is called to clear it.
	 */
	TRACE_GRAPH_NOTRACE_BIT,
};

#define trace_recursion_set(bit)	do { (current)->trace_recursion |= (1<<(bit)); } while (0)
#define trace_recursion_clear(bit)	do { (current)->trace_recursion &= ~(1<<(bit)); } while (0)
#define trace_recursion_test(bit)	((current)->trace_recursion & (1<<(bit)))

#define trace_recursion_depth() \
	(((current)->trace_recursion >> TRACE_GRAPH_DEPTH_START_BIT) & 3)
#define trace_recursion_set_depth(depth) \
	do {								\
		current->trace_recursion &=				\
			~(3 << TRACE_GRAPH_DEPTH_START_BIT);		\
		current->trace_recursion |=				\
			((depth) & 3) << TRACE_GRAPH_DEPTH_START_BIT;	\
	} while (0)

#define TRACE_CONTEXT_BITS	4

#define TRACE_FTRACE_START	TRACE_FTRACE_BIT

#define TRACE_LIST_START	TRACE_INTERNAL_BIT

#define TRACE_CONTEXT_MASK	((1 << (TRACE_LIST_START + TRACE_CONTEXT_BITS)) - 1)

enum {
	TRACE_CTX_NMI,
	TRACE_CTX_IRQ,
	TRACE_CTX_SOFTIRQ,
	TRACE_CTX_NORMAL,
	TRACE_CTX_TRANSITION,
};

static __always_inline int trace_get_context_bit(void)
{
	int bit;

	if (in_interrupt()) {
		if (in_nmi())
			bit = TRACE_CTX_NMI;

		else if (in_irq())
			bit = TRACE_CTX_IRQ;
		else
			bit = TRACE_CTX_SOFTIRQ;
	} else
		bit = TRACE_CTX_NORMAL;

	return bit;
}

static __always_inline int __trace_test_and_set_recursion(int start, bool safe)
{
	unsigned int val = current->trace_recursion;
	int bit;

	bit = trace_get_context_bit() + start;
	if (unlikely(val & (1 << bit))) {
		/*
		 * It could be that preempt_count has not been updated during
		 * a switch between contexts. Allow for a single recursion.
		 */
		bit = start + TRACE_CTX_TRANSITION;
		if (safe || trace_recursion_test(bit))
			return -1;
		trace_recursion_set(bit);
		barrier();
		return bit;
	}

	val |= 1 << bit;
	current->trace_recursion = val;
	barrier();

	return bit;
}

static __always_inline int trace_test_and_set_recursion(int start)
{
	return __trace_test_and_set_recursion(start, false);
}

/*
 * The safe version does not let any recursion happen.
 * The unsafe version will allow for a single recursion to deal with
 * the period during a context switch from normal to interrupt to NMI
 * that may be in the wrong context. But if the caller is expecting
 * this to be safe for grabbing locks, it must use the safe version
 * otherwise it could cause a deadlock. But it may still miss events
 * in the period of context switches, but if it is grabbing locks
 * it shouldn't be tracing in that period anyway.
 */
static __always_inline int trace_test_and_set_recursion_safe(int start)
{
	return __trace_test_and_set_recursion(start, true);
}

static __always_inline void trace_clear_recursion(int bit)
{
	unsigned int val = current->trace_recursion;

	bit = 1 << bit;
	val &= ~bit;

	barrier();
	current->trace_recursion = val;
}

/**
 * ftrace_test_recursion_trylock - tests for recursion in same context
 *
 * Use this for ftrace callbacks. This will detect if the function
 * tracing recursed in the same context (normal vs interrupt),
 *
 * Note, this may allow one nested level of recursion, because of the
 * way interrupts are tracked. If a trace happens at the start of
 * an interrupt before the interrupts state is set, it needs to allow
 * one recursion to handle this case.
 *
 * If this is used to protect locks, use the
 * ftrace_test_recursion_trylock_safe() version instead.
 *
 * Returns: -1 if a recursion happened.
 *           >= 0 if no recursion
 */
static __always_inline int ftrace_test_recursion_trylock(void)
{
	return trace_test_and_set_recursion(TRACE_FTRACE_START);
}

/**
 * ftrace_test_recursion_trylock_safe - tests for recursion in same context
 *
 * Use this for ftrace callbacks. This will detect if the function
 * tracing recursed in the same context (normal vs interrupt),
 *
 * Use this version if you depend on it for grabbing locks.
 * You should also not be tracing in NMI either.
 *
 * This version is the same as the ftrace_test_recursion_trylock() except that
 * it does not allow any recursion. It may produce a false positive if
 * a trace occurs at the start of an interrupt but before the interrupt
 * state is set. Any event that happens in that case will be considered
 * a "recursion".
 *
 * Returns: -1 if a recursion happened.
 *           >= 0 if no recursion
 */
static __always_inline int ftrace_test_recursion_trylock_safe(void)
{
	return trace_test_and_set_recursion_safe(TRACE_FTRACE_START);
}

/**
 * ftrace_test_recursion_unlock - called when function callback is complete
 * @bit: The return of a successful ftrace_test_recursion_trylock()
 *
 * This is used at the end of a ftrace callback.
 */
static __always_inline void ftrace_test_recursion_unlock(int bit)
{
	trace_clear_recursion(bit);
}

#endif /* CONFIG_TRACING */
#endif /* _LINUX_TRACE_RECURSION_H */
