// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/genalloc.h>
#include <linux/slab.h>

#define CHUNK_ORDER	PAGE_SHIFT
#define CHUNK_SIZE	(4 * PAGE_SIZE)

static void genalloc_remove_missing_chunk(struct kunit *test)
{
	struct gen_pool *pool = gen_pool_create(CHUNK_ORDER, -1);

	KUNIT_ASSERT_NOT_NULL(test, pool);
	KUNIT_EXPECT_EQ(test, gen_pool_remove_chunk(pool, 0x10000, CHUNK_SIZE),
			-ENOENT);
	gen_pool_destroy(pool);
}

static void genalloc_remove_busy_chunk(struct kunit *test)
{
	struct gen_pool *pool = gen_pool_create(CHUNK_ORDER, -1);
	unsigned long a;

	KUNIT_ASSERT_NOT_NULL(test, pool);
	KUNIT_ASSERT_EQ(test, gen_pool_add(pool, 0x10000, CHUNK_SIZE, -1), 0);
	a = gen_pool_alloc(pool, PAGE_SIZE);
	KUNIT_ASSERT_NE(test, a, 0UL);
	KUNIT_EXPECT_EQ(test, gen_pool_remove_chunk(pool, 0x10000, CHUNK_SIZE),
			-EBUSY);
	gen_pool_free(pool, a, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, gen_pool_remove_chunk(pool, 0x10000, CHUNK_SIZE), 0);
	gen_pool_destroy(pool);
}

static void genalloc_remove_leaves_other_chunks(struct kunit *test)
{
	struct gen_pool *pool = gen_pool_create(CHUNK_ORDER, -1);
	unsigned long a, other_chunk;

	KUNIT_ASSERT_NOT_NULL(test, pool);
	KUNIT_ASSERT_EQ(test, gen_pool_add(pool, 0x10000, CHUNK_SIZE, -1), 0);
	KUNIT_ASSERT_EQ(test, gen_pool_add(pool, 0x80000, CHUNK_SIZE, -1), 0);
	a = gen_pool_alloc(pool, PAGE_SIZE);
	KUNIT_ASSERT_NE(test, a, 0UL);
	/*
	 * Chunks are searched most-recently-added first, so first-fit put
	 * the allocation in whichever chunk gen_pool_add() saw last. Remove
	 * the other chunk and confirm the allocation survives.
	 */
	other_chunk = a >= 0x80000 ? 0x10000 : 0x80000;
	KUNIT_EXPECT_EQ(test, gen_pool_remove_chunk(pool, other_chunk, CHUNK_SIZE), 0);
	KUNIT_EXPECT_EQ(test, gen_pool_size(pool), (size_t)CHUNK_SIZE);
	KUNIT_EXPECT_TRUE(test, gen_pool_has_addr(pool, a, PAGE_SIZE));
	gen_pool_free(pool, a, PAGE_SIZE);
	gen_pool_destroy(pool);
}

static struct kunit_case genalloc_test_cases[] = {
	KUNIT_CASE(genalloc_remove_missing_chunk),
	KUNIT_CASE(genalloc_remove_busy_chunk),
	KUNIT_CASE(genalloc_remove_leaves_other_chunks),
	{}
};

static struct kunit_suite genalloc_test_suite = {
	.name = "genalloc",
	.test_cases = genalloc_test_cases,
};
kunit_test_suite(genalloc_test_suite);

MODULE_DESCRIPTION("KUnit tests for genalloc");
MODULE_LICENSE("GPL");
