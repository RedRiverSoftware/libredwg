/* ex: ft=c: -*- mode: c; -*- */
/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2026 Red River Software                                    */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/* Reads and frees the test drawings, from r11 to r2018, and writes some of
   them back as r2000 and r2004, on several threads at once. Checks that each
   read gives what the same read gives on one thread (the error code, the
   classes, and each object's type, handle, position, size, owner, layer,
   reactors and extended data count), and that each write gives the same
   bytes.

   dwg_read_file, dwg_write_file and dwg_free keep their per-call state (the
   loop counters of the generated functions, the current version, r2007's
   section buffers) in thread-local storage. Where it is shared instead, one
   thread moves another thread's array index past the end of its array, and
   this test usually crashes. Skipped where there are no threads. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

// the library sets loglevel from each document's opts
static int test_loglevel;

#include "config.h"
#include "dwg.h"
#include "tests_common.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <process.h>
#  define HAVE_TEST_THREADS
#elif defined(HAVE_PTHREAD_H)
#  include <pthread.h>
#  define HAVE_TEST_THREADS
#endif

#ifdef HAVE_TEST_THREADS

#  define NUM_THREADS 8
#  define ROUNDS 2
#  define WRITE_PASSES 2 // per round
/* The decoder needs more stack than some platforms give a new thread. */
#  define THREAD_STACK_SIZE (8 * 1024 * 1024)

typedef struct
{
  const char *file; // under test/test-data
  int write;        // read, then write back in the same version
  int little_endian_only; // not read on big-endian machines yet (P1-09-FU3)
} Job;

/* Reads: one of each decoder, r11, r13-r2000, r2004 and r2010+ sections,
   r2007. Then writes: r2000 and r2004. */
static const Job jobs[] = {
  { "r11/ACEB10.dwg", 0, 0 },     { "example_r14.dwg", 0, 0 },
  { "example_2000.dwg", 0, 0 },   { "example_2004.dwg", 0, 0 },
  { "2004/Dynblocks.dwg", 0, 0 }, { "example_2007.dwg", 0, 1 },
  { "example_2010.dwg", 0, 0 },   { "example_2013.dwg", 0, 0 },
  { "example_2018.dwg", 0, 0 },
#  ifdef USE_WRITE
  { "example_2000.dwg", 1, 0 },   { "example_2004.dwg", 1, 0 },
  { "2004/Dynblocks.dwg", 1, 0 },
#  endif
};
#  define NUM_JOBS (sizeof (jobs) / sizeof (jobs[0]))

static char paths[NUM_JOBS][512];
static unsigned num_reads; // the jobs before the first write
static int left_out[NUM_JOBS];

typedef struct
{
  int error;
  BITCODE_BL num_objects;
  BITCODE_BS num_classes;
  size_t size; // of the written file
  uint64_t hash;
} Job_Result;

/* Each job's result on the main thread */
static Job_Result expected[NUM_JOBS];

typedef struct
{
  unsigned id;
  unsigned runs;
  unsigned mismatches;
  unsigned first_mismatch; // index into jobs, + 1
} Worker;

#  define FNV_OFFSET UINT64_C (0xcbf29ce484222325)
#  define FNV_PRIME UINT64_C (0x100000001b3)

static int
find_fixture (const char *name, char *path, size_t size)
{
  const char *dirs[3];
  struct stat attrib;
  dirs[0] = getenv ("top_srcdir");
  dirs[1] = "../..";
  dirs[2] = "../../..";
  for (size_t i = 0; i < sizeof (dirs) / sizeof (dirs[0]); i++)
    {
      if (!dirs[i])
        continue;
      snprintf (path, size, "%s/test/test-data/%s", dirs[i], name);
      if (!stat (path, &attrib))
        return 1;
    }
  return 0;
}

/* FNV-1a over the 8 bytes of v */
static uint64_t
mix (uint64_t hash, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    {
      hash ^= (v >> (i * 8)) & 0xff;
      hash *= FNV_PRIME;
    }
  return hash;
}

static uint64_t
mix_string (uint64_t hash, const char *s)
{
  if (!s)
    return mix (hash, 0);
  for (; *s; s++)
    hash = mix (hash, (unsigned char)*s);
  return mix (hash, 0);
}

static uint64_t
mix_ref (uint64_t hash, const Dwg_Object_Ref *ref)
{
  return mix (hash, ref ? ref->absolute_ref : 0);
}

static uint64_t
mix_reactors (uint64_t hash, BITCODE_BL num_reactors,
              BITCODE_H *const reactors)
{
  hash = mix (hash, num_reactors);
  if (reactors)
    for (BITCODE_BL i = 0; i < num_reactors; i++)
      hash = mix_ref (hash, reactors[i]);
  return hash;
}

static uint64_t
hash_document (const Dwg_Data *dwg)
{
  uint64_t hash = FNV_OFFSET;
  hash = mix (hash, dwg->header.version);
  hash = mix (hash, dwg->header.from_version);
  for (BITCODE_BS i = 0; i < dwg->num_classes; i++)
    {
      hash = mix (hash, dwg->dwg_class[i].number);
      hash = mix_string (hash, dwg->dwg_class[i].dxfname);
    }
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      const Dwg_Object *obj = &dwg->object[i];
      hash = mix (hash, obj->fixedtype);
      hash = mix (hash, obj->handle.value);
      hash = mix (hash, obj->address);
      hash = mix (hash, obj->size);
      hash = mix (hash, obj->bitsize);
      if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity)
        {
          const Dwg_Object_Entity *ent = obj->tio.entity;
          hash = mix_ref (hash, ent->ownerhandle);
          hash = mix_ref (hash, ent->layer);
          hash = mix_reactors (hash, ent->num_reactors, ent->reactors);
          hash = mix (hash, ent->num_eed);
        }
      else if (obj->supertype == DWG_SUPERTYPE_OBJECT && obj->tio.object)
        {
          const Dwg_Object_Object *o = obj->tio.object;
          hash = mix_ref (hash, o->ownerhandle);
          hash = mix_reactors (hash, o->num_reactors, o->reactors);
          hash = mix (hash, o->num_eed);
        }
    }
  return hash;
}

#  ifdef USE_WRITE
/* The size and hash of a written file, then removed */
static int
hash_file (const char *path, Job_Result *result)
{
  unsigned char buf[4096];
  size_t n;
  uint64_t hash = FNV_OFFSET;
  FILE *fp = fopen (path, "rb");
  if (!fp)
    return 1;
  result->size = 0;
  while ((n = fread (buf, 1, sizeof (buf), fp)) > 0)
    {
      for (size_t i = 0; i < n; i++)
        {
          hash ^= buf[i];
          hash *= FNV_PRIME;
        }
      result->size += n;
    }
  fclose (fp);
  remove (path);
  result->hash = hash;
  return 0;
}
#  endif

/* out names the file a write job writes, which the job removes again */
static void
run_job (unsigned j, const char *out, Job_Result *result)
{
  Dwg_Data dwg;

  memset (&dwg, 0, sizeof (dwg));
  memset (result, 0, sizeof (*result));
  result->error = dwg_read_file (paths[j], &dwg);
  result->num_objects = dwg.num_objects;
  result->num_classes = dwg.num_classes;
#  ifdef USE_WRITE
  if (jobs[j].write)
    {
      if (result->error < DWG_ERR_CRITICAL)
        {
          remove (out); // dwg_write_file refuses to overwrite a file
          result->error = dwg_write_file (out, &dwg);
        }
      if (result->error < DWG_ERR_CRITICAL && hash_file (out, result))
        result->error = DWG_ERR_IOERROR;
    }
  else
#  endif
    result->hash = hash_document (&dwg);
  (void)out;
  dwg_free (&dwg);
}

static int
same_result (const Job_Result *a, const Job_Result *b)
{
  return a->error == b->error && a->num_objects == b->num_objects
         && a->num_classes == b->num_classes && a->size == b->size
         && a->hash == b->hash;
}

static void
check_job (Worker *w, unsigned j, const char *out)
{
  Job_Result result;
  if (left_out[j])
    return;
  run_job (j, out, &result);
  w->runs++;
  if (!same_result (&result, &expected[j]))
    {
      w->mismatches++;
      if (!w->first_mismatch)
        w->first_mismatch = j + 1;
    }
}

static unsigned
runs_per_thread (void)
{
  unsigned runs = 0;
  for (unsigned j = 0; j < NUM_JOBS; j++)
    if (!left_out[j])
      runs += j < num_reads ? 1 : WRITE_PASSES;
  return ROUNDS * runs;
}

static int
is_big_endian (void)
{
  const uint16_t one = 1;
  return *(const unsigned char *)&one == 0;
}

/* In each round, a thread reads every file, starting at a different file
   from the other threads, so that different decoders run at the same time.
   Then the threads write, all of them the same file at about the same time,
   which shared loop counters in the encoder need in order to show. */
static void
run_worker (Worker *w)
{
  const unsigned num_writes = (unsigned)NUM_JOBS - num_reads;
  char out[64];
  snprintf (out, sizeof (out), "threads_test_%u.dwg", w->id);
  for (unsigned round = 0; round < ROUNDS; round++)
    {
      for (unsigned k = 0; k < num_reads; k++)
        check_job (w, (w->id + k) % num_reads, out);
      for (unsigned k = 0; k < WRITE_PASSES * num_writes; k++)
        check_job (w, num_reads + k % num_writes, out);
    }
}

#  ifdef _WIN32
typedef HANDLE Test_Thread;

static unsigned __stdcall
thread_main (void *arg)
{
  run_worker ((Worker *)arg);
  return 0;
}

static int
start_thread (Test_Thread *thread, Worker *w)
{
  *thread = (HANDLE)_beginthreadex (NULL, THREAD_STACK_SIZE, thread_main, w,
                                    STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  return *thread ? 0 : 1;
}

static int
join_thread (Test_Thread thread)
{
  const DWORD status = WaitForSingleObject (thread, INFINITE);
  CloseHandle (thread);
  return status == WAIT_OBJECT_0 ? 0 : 1;
}
#  else
typedef pthread_t Test_Thread;

static void *
thread_main (void *arg)
{
  run_worker ((Worker *)arg);
  return NULL;
}

static int
start_thread (Test_Thread *thread, Worker *w)
{
  pthread_attr_t attr;
  int error = pthread_attr_init (&attr);
  if (error)
    return error;
  error = pthread_attr_setstacksize (&attr, THREAD_STACK_SIZE);
  if (!error)
    error = pthread_create (thread, &attr, thread_main, w);
  pthread_attr_destroy (&attr);
  return error;
}

static int
join_thread (Test_Thread thread)
{
  return pthread_join (thread, NULL);
}
#  endif

#endif /* HAVE_TEST_THREADS */

int
main (int argc, char *argv[])
{
#ifdef HAVE_TEST_THREADS
  Test_Thread threads[NUM_THREADS];
  Worker workers[NUM_THREADS];
  unsigned started = 0;
  unsigned runs = 0;
#endif
  test_loglevel = is_make_silent () ? 0 : 2; // print ok
  loglevel = test_loglevel;
  (void)argc;
  (void)argv;

#ifndef HAVE_TEST_THREADS
  printf ("SKIP: no threads on this platform\n");
  return 77;
#else
  while (num_reads < NUM_JOBS && !jobs[num_reads].write)
    num_reads++;
  for (unsigned j = 0; j < NUM_JOBS; j++)
    {
      const char *what = jobs[j].write ? "write" : "read";
      if (!find_fixture (jobs[j].file, paths[j], sizeof (paths[j])))
        {
          fail ("test/test-data/%s not found", jobs[j].file);
          return 1;
        }
      run_job (j, "threads_test.dwg", &expected[j]);
      loglevel = test_loglevel;
      if (jobs[j].little_endian_only && is_big_endian ())
        {
          if (expected[j].error >= DWG_ERR_CRITICAL)
            {
              todo ("%s: not read on big-endian machines (P1-09-FU3), "
                    "left out",
                    jobs[j].file);
              left_out[j] = 1;
            }
          else
            fail ("%s: read on a big-endian machine, so it need not be "
                  "left out there any more (P1-09-FU3)",
                  jobs[j].file);
        }
      else if (expected[j].error >= DWG_ERR_CRITICAL
               || !expected[j].num_objects
               || (jobs[j].write && !expected[j].size))
        fail ("%s: %s on one thread: error 0x%x, %u objects", jobs[j].file,
              what, (unsigned)expected[j].error,
              (unsigned)expected[j].num_objects);
      else
        ok ("%s: %s on one thread: error 0x%x, %u objects", jobs[j].file,
            what, (unsigned)expected[j].error,
            (unsigned)expected[j].num_objects);
    }
  if (numfailed ())
    return 1;

  memset (workers, 0, sizeof (workers));
  for (unsigned t = 0; t < NUM_THREADS; t++)
    {
      workers[t].id = t;
      if (start_thread (&threads[t], &workers[t]))
        break;
      started++;
    }
  for (unsigned t = 0; t < started; t++)
    if (join_thread (threads[t]))
      fail ("thread %u: join failed", t);
  loglevel = test_loglevel;
  if (started < NUM_THREADS)
    fail ("started %u of %u threads", started, NUM_THREADS);

  for (unsigned t = 0; t < started; t++)
    {
      const Worker *w = &workers[t];
      runs += w->runs;
      if (w->runs != runs_per_thread ())
        fail ("thread %u: %u of %u jobs ran", t, w->runs, runs_per_thread ());
      else if (w->mismatches)
        fail ("thread %u: %u of %u jobs differ from the same job on one "
              "thread, the first a %s of %s",
              t, w->mismatches, w->runs,
              jobs[w->first_mismatch - 1].write ? "write" : "read",
              jobs[w->first_mismatch - 1].file);
      else
        ok ("thread %u: %u jobs, each the same as on one thread", t, w->runs);
    }
  if (!numfailed ())
    ok ("%u threads: %u reads and writes, each the same as on one thread",
        started, runs);
  return numfailed () ? 1 : 0;
#endif
}
