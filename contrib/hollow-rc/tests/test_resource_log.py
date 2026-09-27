"""Exercise the real diagnostic header without MPI or RDMA hardware."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HEADER_DIR = ROOT / "src/mpid/ch3/channels/mrail/src/gen2"
HARNESS = r'''
#include <errno.h>
#include <time.h>
static unsigned int clock_reads;
static inline int test_clock_gettime(clockid_t clock, struct timespec *now)
{
    (void)clock;
    __atomic_add_fetch(&clock_reads, 1, __ATOMIC_RELAXED);
    now->tv_sec = 123;
    now->tv_nsec = 0;
    errno = ERANGE; /* Ensure diagnostic calls restore the caller's errno. */
    return 0;
}
#define clock_gettime test_clock_gettime
#define MV2_RESOURCE_LOG_IMPLEMENTATION
#include "mv2_resource_log.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>

static void *events(void *unused)
{
    int i;
    (void)unused;
    for (i = 0; i < 100; ++i) {
        errno = EDOM;
        MV2_RESOURCE_LOG(MV2_RES_VBUF_GROW, "pool=%d", 2);
        assert(errno == EDOM);
    }
    return NULL;
}

int main(void)
{
    int touches = 0;
    int conditions = 0;
    pthread_t a, b;
    errno = EDOM;
    mv2_resource_log_init(7);
    assert(errno == EDOM);
    MV2_RESOURCE_LOG(MV2_RES_SRQ_POST_SHORT, "ret=%d", ++touches);
    MV2_RESOURCE_LOG_IF(++conditions == 999, MV2_RES_FORCED_RNDV, "unused=%d", 1);
    assert(errno == EDOM);
#if MV2_ENABLE_RESOURCE_LOG
    assert(touches == !!mv2_resource_log_enabled);
    assert(conditions == !!mv2_resource_log_enabled);
#else
    assert(touches == 0);
    assert(conditions == 0);
#endif
    assert(!pthread_create(&a, NULL, events, NULL));
    assert(!pthread_create(&b, NULL, events, NULL));
    pthread_join(a, NULL);
    pthread_join(b, NULL);
#if MV2_ENABLE_RESOURCE_LOG
    if (mv2_resource_log_enabled) {
        unsigned int growth_lines = mv2_res_max_lines < 8 ? mv2_res_max_lines : 8;
        /* One initialization clock, one short-post log, only sampled growth
         * events. No clock reads on the other 200-growth_lines events.
         */
        assert(clock_reads == growth_lines + 2);
    } else
#endif
    {
        assert(clock_reads == 0);
    }
    return 0;
}
'''


class ResourceLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mv2-resource-test-")
        cls.addClassCleanup(cls.tmp.cleanup)
        source = Path(cls.tmp.name) / "test.c"
        source.write_text(HARNESS)
        cls.bins = {}
        for enabled in (0, 1):
            binary = Path(cls.tmp.name) / ("test-" + str(enabled))
            subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-pthread", "-I" + str(HEADER_DIR),
                            "-DMV2_ENABLE_RESOURCE_LOG=" + str(enabled),
                            str(source), "-o", str(binary)], check=True)
            cls.bins[enabled] = binary

    def run_log(self, compiled=1, **settings):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("MV2_RESOURCE_LOG")}
        env.update(settings)
        result = subprocess.run([str(self.bins[compiled])], env=env,
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "")
        return result.stderr

    def test_default_off(self):
        self.assertEqual(self.run_log(), "")

    def test_compiled_out(self):
        self.assertEqual(self.run_log(compiled=0, MV2_RESOURCE_LOG="1"), "")

    def test_rank_filter(self):
        self.assertEqual(self.run_log(MV2_RESOURCE_LOG="1", MV2_RESOURCE_LOG_RANK="2"), "")

    def test_bounded_thread_safe_counts(self):
        log = self.run_log(MV2_RESOURCE_LOG="1", MV2_RESOURCE_LOG_RANK="7",
                           MV2_RESOURCE_LOG_MAX="4", MV2_TRANSPORT_MODE="ordinary")
        rows = log.splitlines()
        events = [r for r in rows if r.startswith("MV2_RESOURCE_EVENT") and "event=vbuf_grow" in r]
        self.assertEqual(len(events), 4)
        self.assertEqual({int(r.split(" count=")[1].split()[0]) for r in events}, {1, 2, 4, 8})
        self.assertIn("event=vbuf_grow count=200", log)
        self.assertIn("event=srq_post_short count=1", log)
        self.assertTrue(all("rank=7" in r and "mode=ordinary" in r for r in rows))

    def test_invalid_settings_fail_closed(self):
        for setting in ({"MV2_RESOURCE_LOG": "yes"},
                        {"MV2_RESOURCE_LOG_MAX": "0"},
                        {"MV2_RESOURCE_LOG_MAX": "65"},
                        {"MV2_RESOURCE_LOG_RANK": "-2"},
                        {"MV2_RESOURCE_LOG_RANK": "nonsense"}):
            env = dict(MV2_RESOURCE_LOG="1")
            env.update(setting)
            log = self.run_log(**env)
            self.assertIn("diagnostics_disabled=1", log)
            self.assertNotIn("MV2_RESOURCE_EVENT", log)

    def test_line_budget_boundaries(self):
        for maximum, expected_lines in ((1, 1), (8, 8), (64, 8)):
            with self.subTest(maximum=maximum):
                log = self.run_log(MV2_RESOURCE_LOG="1",
                                   MV2_RESOURCE_LOG_MAX=str(maximum))
                events = [r for r in log.splitlines()
                          if r.startswith("MV2_RESOURCE_EVENT") and "event=vbuf_grow" in r]
                self.assertEqual(len(events), expected_lines)
                self.assertIn("event=vbuf_grow count=200", log)


if __name__ == "__main__":
    unittest.main()
