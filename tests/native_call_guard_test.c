#include "native_call_guard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define TARGET 0x80002000u
#define CONT 0x80001004u
typedef struct Fixture {
  s64 deadline;
  u64 charged, watermark[8];
  unsigned depth, enters, posts, bodies, continuations;
  int hook, fallback_chunk;
} Fixture;
static void flush(CPUState* cpu, Fixture* f)
{
  CHECK(cpu->downcount <= 0);
  u64 charge = (u64)-cpu->downcount;
  f->charged += charge;
  f->deadline -= (s64)charge;
  cpu->timebase += charge;
  cpu->downcount = 0;
}
/* Models checkpoints; fallback chunks are deliberately ineligible. */
static int checkpoint(CPUState* cpu, u32 target, u32 cont, u32 phase, void* user)
{
  Fixture* f = user;
  CHECK(target == TARGET && cont == CONT);
  flush(cpu, f);
  if (phase == 0) {
    ++f->enters;
    if (f->hook || f->fallback_chunk || cpu->exception || f->deadline <= 0) return 0;
    CHECK(f->depth < 8);
    f->watermark[f->depth++] = f->charged;
    return 1;
  }
  CHECK(phase == 1 && f->depth > 0);
  ++f->posts;
  if (f->watermark[--f->depth] == f->charged) {
    cpu->downcount = -1;
    flush(cpu, f);
  }
  return !f->hook && !cpu->exception && f->deadline > 0;
}
static CPUState reset(Fixture* f)
{
  CPUState cpu;
  memset(&cpu, 0, sizeof(cpu));
  memset(f, 0, sizeof(*f));
  f->deadline = 100;
  cpu.pc = TARGET; cpu.lr = CONT;
  staticrecomp_set_call_guard_v2(checkpoint, f);
  staticrecomp_native_begin_dispatch();
  return cpu;
}
static int enter(CPUState* cpu)
{
  cpu->pc = TARGET; cpu->lr = CONT;
  return staticrecomp_native_call_enter(cpu, TARGET, CONT);
}
static int finish(CPUState* cpu, Fixture* f)
{
  int allowed = staticrecomp_native_call_finish(cpu, TARGET, CONT);
  if (allowed) ++f->continuations;
  return allowed;
}
int main(void)
{
  Fixture f;
  CPUState cpu = reset(&f);
  staticrecomp_set_call_guard_v2(NULL, NULL);
  CHECK(!enter(&cpu) && f.enters == 0);
  cpu = reset(&f); cpu.pc++;
  CHECK(!staticrecomp_native_call_enter(&cpu, TARGET, CONT) && f.enters == 0);
  cpu = reset(&f); cpu.lr++;
  CHECK(!staticrecomp_native_call_enter(&cpu, TARGET, CONT) && f.enters == 0);
  cpu = reset(&f); cpu.downcount = -3;
  CHECK(enter(&cpu)); cpu.pc = CONT;
  CHECK(finish(&cpu, &f) && f.charged == 4 && cpu.timebase == 4 && f.deadline == 96 && f.posts == 1 && f.depth == 0);
  /* Nested checkpoints conserve caller and callee charges without extra 1. */
  cpu = reset(&f); cpu.downcount = -2;
  CHECK(enter(&cpu)); cpu.downcount = -3;
  CHECK(enter(&cpu)); cpu.downcount = -5; cpu.pc = CONT;
  CHECK(finish(&cpu, &f)); cpu.downcount = -7; cpu.pc = CONT;
  CHECK(finish(&cpu, &f) && f.charged == 17 && cpu.timebase == 17 && f.posts == 2 && f.depth == 0);
  /* A nested deadline denial must unwind even with coincidental outer PC. */
  cpu = reset(&f); f.deadline = 3;
  CHECK(enter(&cpu)); cpu.downcount = -3;
  CHECK(!enter(&cpu)); cpu.pc = CONT;
  CHECK(!finish(&cpu, &f) && f.charged == 3 && f.posts == 1 && f.depth == 0 && f.continuations == 0);
  /* Hook denial has positive deadline, proving sticky state is independent. */
  cpu = reset(&f); CHECK(enter(&cpu)); f.hook = 1;
  CHECK(!enter(&cpu)); f.hook = 0; cpu.pc = CONT;
  CHECK(!finish(&cpu, &f) && f.deadline > 0 && f.posts == 1 && f.continuations == 0);
  staticrecomp_native_begin_dispatch(); CHECK(enter(&cpu)); cpu.pc = CONT;
  CHECK(finish(&cpu, &f) && f.posts == 2);
  cpu = reset(&f); CHECK(enter(&cpu)); f.hook = 1; cpu.pc = CONT;
  CHECK(!finish(&cpu, &f) && f.posts == 1 && f.depth == 0 && f.continuations == 0);
  cpu = reset(&f); CHECK(enter(&cpu)); cpu.pc = 0x80003000u;
  CHECK(!finish(&cpu, &f) && f.posts == 1 && cpu.pc == 0x80003000u);
  cpu = reset(&f); CHECK(enter(&cpu)); cpu.pc = CONT; cpu.exception = PPC_EXC_PROGRAM;
  CHECK(!finish(&cpu, &f) && f.posts == 1 && f.continuations == 0);
  cpu = reset(&f); f.deadline = 2; CHECK(enter(&cpu)); cpu.downcount = -2; cpu.pc = CONT;
  CHECK(!finish(&cpu, &f) && f.charged == 2 && f.deadline == 0 && f.posts == 1 && f.continuations == 0);
  cpu = reset(&f);
  for (unsigned i = 0; i < 8; ++i) CHECK(enter(&cpu));
  CHECK(!enter(&cpu) && f.enters == 8 && f.depth == 8);
  for (unsigned i = 0; i < 8; ++i) { cpu.pc = CONT; CHECK(!finish(&cpu, &f)); }
  CHECK(f.posts == 8 && f.depth == 0 && f.charged == 1 && f.continuations == 0);
  cpu = reset(&f); f.fallback_chunk = 1;
  if (enter(&cpu)) ++f.bodies;
  CHECK(f.bodies == 0 && f.posts == 0 && f.depth == 0);
  cpu = reset(&f); staticrecomp_set_call_guard_v2(NULL, NULL);
  CHECK(!enter(&cpu) && f.enters == 0);
  puts("Native call guard V2 tests passed");
  return 0;
}
