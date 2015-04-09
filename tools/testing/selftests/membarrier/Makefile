CFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -I../../../../include/uapi/
CFLAGS += -I../../../../include/

all:
	gcc $(CFLAGS) membarrier_test.c -o membarrier_test

run_tests: all
	gcc $(CFLAGS) membarrier_test.c -o membarrier_test
	@./membarrier_test || echo "membarrier_test: [FAIL]"

clean:
	$(RM) membarrier_test
