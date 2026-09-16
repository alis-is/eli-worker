#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include "lua.h"
#include "lauxlib.h"
#include "eli_xfer.h"
#include "c11threads.h"

/* Exercise the real helper with a 32-bit clock even on 64-bit build hosts. */
#ifdef ELI_TEST_TIME32
struct test_timespec { int32_t tv_sec; long tv_nsec; };
#define time_t int32_t
#define timespec test_timespec
#endif
static struct timespec now;
static int test_clock_ok = 1;
static int test_timespec_get(struct timespec *value, int base)
{
	assert(base == TIME_UTC);
	if (!test_clock_ok) {
		return 0;
	}
	*value = now;
	return base;
}
#define timespec_get test_timespec_get
#include "eli_worker_internal.h"
#undef timespec_get

int main(void)
{
	struct timespec deadline;
	const time_t maximum = ELI_WORKER_TIME_MAX;
	now.tv_sec = 1700000000;
	now.tv_nsec = 999000000;
	eli_worker_deadline(1001, &deadline);
	assert(deadline.tv_sec == 1700000002 && deadline.tv_nsec == 0);
	eli_worker_deadline(INT64_MAX, &deadline);
#ifdef ELI_TEST_TIME32
	assert(deadline.tv_sec == INT32_MAX && deadline.tv_nsec == 999999999);
#else
	if ((uintmax_t)maximum > INT64_MAX / 1000 + 1700000001ULL) {
		assert(deadline.tv_sec == 1700000001 + INT64_MAX / 1000);
		assert(deadline.tv_nsec == 806000000);
	} else {
		assert(deadline.tv_sec == maximum && deadline.tv_nsec == 999999999);
	}
#endif
	now.tv_sec = maximum - 1;
	eli_worker_deadline(1, &deadline);
	assert(deadline.tv_sec == maximum && deadline.tv_nsec == 0);
	eli_worker_deadline(1001, &deadline);
	assert(deadline.tv_sec == maximum && deadline.tv_nsec == 999999999);
	now.tv_sec = maximum;
	eli_worker_deadline(1, &deadline);
	assert(deadline.tv_sec == maximum && deadline.tv_nsec == 999999999);

	/* A failing clock must not build a deadline from uninitialized fields. */
	test_clock_ok = 0;
	eli_worker_deadline(1000, &deadline);
	assert(deadline.tv_sec == 1 && deadline.tv_nsec == 0);
	test_clock_ok = 1;

#ifndef ELI_TEST_TIME32
	/* An expired UTC deadline must time out, not wait for monotonic uptime
	 * to reach the Unix epoch. CTest's watchdog bounds that regression. */
	cnd_t cond;
	mtx_t mutex;
	assert(cnd_init(&cond) == thrd_success);
	assert(mtx_init(&mutex, mtx_plain) == thrd_success);
	assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
	deadline.tv_sec--;
	assert(mtx_lock(&mutex) == thrd_success);
	assert(cnd_timedwait(&cond, &mutex, &deadline) == thrd_timedout);
	assert(mtx_unlock(&mutex) == thrd_success);
	mtx_destroy(&mutex);
	cnd_destroy(&cond);
#endif
	return 0;
}
