#define HASH_TEST_C
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "common.h"
#include "hash.h"
#define DWG_LOGLEVEL loglevel
#include "logging.h"

/* hash.c itself, so that its calloc can fail: while fail_calloc is set. */
static int fail_calloc;

static void *
test_calloc (const size_t n, const size_t size)
{
  return fail_calloc ? NULL : calloc (n, size);
}

#define calloc(n, size) test_calloc (n, size)
#include "hash.c"
#undef calloc

#include "tests_common.h"

#define MAX_SIZE 2453916
// 1 for full pressure, 5 for light pressure
#define PRESSURE_FACTOR 1

static inline uint32_t
maxrand (int max)
{
  uint32_t rnd = (uint32_t)rand ();
  return rnd % (uint32_t)max;
}

/* A hash whose buckets can't be allocated is no hash: hash_new returns
   NULL, not one that hash_get and hash_set would read through a NULL
   bucket array. */
static void
test_new_out_of_memory (void)
{
  dwg_inthash *hash;
  fail_calloc = 1;
  hash = hash_new (16);
  fail_calloc = 0;
  if (!hash)
    ok ("hash_new returns NULL when it can't allocate its buckets");
  else
    {
      fail ("hash_new returns a hash with buckets %p when it can't allocate "
            "them",
            (void *)hash->array);
      hash_free (hash);
    }
}

int
main (int argc, char const *argv[])
{
  const int max = MAX_SIZE;
  int i, wrong = 0;
  uint32_t first_key = 0;
  uint64_t first_value = 0;
  dwg_inthash *hash;

  loglevel = is_make_silent () ? 0 : 2;
  test_new_out_of_memory ();

  hash = hash_new (max);
  if (!hash)
    {
      fail ("hash_new (%d) failed", max);
      return 1;
    }
  for (i = 1; i < max / PRESSURE_FACTOR; i++)
    {
      int32_t rnd = i; // we need to ensure full coverage.
      // i = maxrand(i)+1; for a more realistic workload
      hash_set (hash, rnd, rnd + 1);
    }
  ok ("hash size(%d) => %" PRIu64, max, hash->size);

  for (i = 1; i < max / PRESSURE_FACTOR; i++)
    {
      uint32_t rnd = maxrand (i) + 1;
      uint64_t v = hash_get (hash, rnd);
      if (v != rnd + 1 && !wrong++)
        {
          first_key = rnd;
          first_value = v;
        }
    }
  if (!wrong)
    ok ("hash_get of %d keys", max / PRESSURE_FACTOR - 1);
  else
    fail ("hash_get: %d of %d keys wrong, the first: hash_get(%u) => %" PRIu64,
          wrong, max / PRESSURE_FACTOR - 1, first_key, first_value);

  hash_free (hash);
  return numfailed () ? 1 : 0;
}
