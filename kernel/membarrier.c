/*
 * Copyright (C) 2010, 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * membarrier system call
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/syscalls.h>
#include <linux/membarrier.h>

static int membarrier_validate_flags(int flags)
{
	/* Check for unrecognized flags. */
	if (flags & ~MEMBARRIER_QUERY)
		return -EINVAL;
	return 0;
}

#ifdef CONFIG_SMP

/*
 * sys_membarrier - issue memory barrier on all running threads
 * @flags: MEMBARRIER_QUERY:
 *             Query whether the rest of the specified flags are supported,
 *             without performing synchronization.
 *
 * return values: Returns -EINVAL if the flags are incorrect. Testing
 * for kernel sys_membarrier support can be done by checking for -ENOSYS
 * return value.  Return value of 0 indicates success. For a given set
 * of flags on a given kernel, this system call will always return the
 * same value. It is therefore correct to check the return value only
 * once during a process lifetime, setting MEMBARRIER_QUERY to only
 * check if the flags are supported, without performing any
 * synchronization.
 *
 * This system call executes a memory barrier on all running threads.
 * Upon completion, the caller thread is ensured that all running
 * threads have passed through a state where all memory accesses to
 * user-space addresses match program order. (non-running threads are de
 * facto in such a state.)
 *
 * On uniprocessor systems, this system call simply returns 0 after
 * validating the arguments, so user-space knows it is implemented.
 */
SYSCALL_DEFINE1(membarrier, int, flags)
{
	int retval;

	retval = membarrier_validate_flags(flags);
	if (retval)
		goto end;
	if (unlikely(flags & MEMBARRIER_QUERY) || num_online_cpus() == 1)
		goto end;
	synchronize_sched();
end:
	return retval;
}

#else /* !CONFIG_SMP */

SYSCALL_DEFINE1(membarrier, int, flags)
{
	return membarrier_validate_flags(flags);
}

#endif /* CONFIG_SMP */
