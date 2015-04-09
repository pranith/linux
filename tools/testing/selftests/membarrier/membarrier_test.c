#define _GNU_SOURCE
#define __EXPORTED_HEADERS__

#include <linux/membarrier.h>
#include <asm-generic/unistd.h>
#include <sys/syscall.h>
#include <stdio.h>

#include "../kselftest.h"

static int sys_membarrier(int flags)
{
	return syscall(__NR_membarrier, flags);
}

static void test_membarrier_fail()
{
	int flags = 1;

	if (sys_membarrier(flags) != -1)
		ksft_exit_fail();
}

static void test_membarrier_success()
{
	int flags = 0;

	if (sys_membarrier(flags) != 0)
		ksft_exit_fail();
}

static void test_membarrier()
{
	test_membarrier_fail();
	test_membarrier_success();
}

static int test_membarrier_exists()
{
	int flags = MEMBARRIER_QUERY;

	if (sys_membarrier(flags) != 0)
		return ksft_exit_fail();

	return 1;
}

int main(int argc, char **argv)
{
	printf("membarrier: ");
	if (test_membarrier_exists()) {
		printf("IMPLEMENTED\n");
		test_membarrier();
	} else
		printf("NOT IMPLEMENTED!\n");

	printf("membarrier: DONE\n");

	return ksft_exit_pass();
}
