#include "sdk_leaf_guard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static const u32 targets[] = {0x800A2D38u, 0x800A2D64u, 0x800A2D98u, 0x800A37CCu, 0x801A85F0u};
static const u32 continuation = 0x80001004u;
enum { NORMAL, DECLINE, INSTALL_HOOK, EXCEPTION, OTHER_PC, SPEND_BUDGET };
static int mode, calls, guard_calls, deny, pending_hook;
static int token;
static int guard(CPUState* cpu, u32 target, u32 next, void* user)
{
  CHECK(cpu != NULL && user == &token);
  CHECK(target == targets[0] || target == targets[1] || target == targets[2] || target == targets[3] || target == targets[4]);
  CHECK(next == continuation);
  ++guard_calls;
  return !deny && !pending_hook;
}
static int leaf(CPUState* cpu)
{
  ++calls;
  if (mode == DECLINE) return 0;
  cpu->gpr[3] += 7;
  cpu->ram[0] = 42;
  cpu->pc = cpu->lr;
  if (mode == INSTALL_HOOK) pending_hook = 1;
  if (mode == EXCEPTION) { cpu->exception = PPC_EXC_PROGRAM; cpu->pc = 0x80000700u; }
  if (mode == OTHER_PC) cpu->pc = 0x80002000u;
  if (mode == SPEND_BUDGET) cpu->downcount = -256;
  return 1;
}
int dolrecomp_native_psmtx_identity(CPUState* cpu) { return leaf(cpu); }
int dolrecomp_native_psmtx_copy(CPUState* cpu) { return leaf(cpu); }
int dolrecomp_native_psmtx_concat(CPUState* cpu) { return leaf(cpu); }
int dolrecomp_native_psmtx_mult_vec(CPUState* cpu) { return leaf(cpu); }
int dolrecomp_native_hsd_mtx_scaled_add(CPUState* cpu) { return leaf(cpu); }
static CPUState fresh(u8* ram, u32 target)
{
  CPUState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.pc = target; cpu.lr = continuation; cpu.ram = ram; cpu.ram_size = 1;
  ram[0] = 0;
  mode = NORMAL; calls = guard_calls = deny = pending_hook = 0;
  staticrecomp_set_sdk_guard_v1(guard, &token);
  return cpu;
}
/* Mirrors emitted caller control flow: only result 1 reaches continuation. */
static int caller(CPUState* cpu, u32 target)
{
  int result = staticrecomp_try_sdk_leaf(cpu, target, continuation);
  if (result == 1) cpu->gpr[4]++;
  return result;
}
static void denied_unchanged(CPUState* cpu, u32 target)
{
  CPUState before = *cpu;
  CHECK(caller(cpu, target) == 0);
  CHECK(calls == 0 && memcmp(cpu, &before, sizeof(before)) == 0 && cpu->ram[0] == 0);
}
int main(void)
{
  u8 ram[1], chassis_ram[1];
  CPUState cpu = fresh(ram, targets[0]);
  staticrecomp_set_sdk_guard_v1(NULL, NULL); denied_unchanged(&cpu, targets[0]);
  cpu = fresh(ram, 0x80009900u); denied_unchanged(&cpu, cpu.pc); CHECK(guard_calls == 0);
  cpu = fresh(ram, targets[0]); deny = 1; denied_unchanged(&cpu, targets[0]); CHECK(guard_calls == 1);
  cpu = fresh(ram, targets[0]); cpu.pc++; denied_unchanged(&cpu, targets[0]);
  cpu = fresh(ram, targets[0]); cpu.lr++; denied_unchanged(&cpu, targets[0]);
  cpu = fresh(ram, targets[0]); cpu.exception = PPC_EXC_PROGRAM; denied_unchanged(&cpu, targets[0]);
  cpu = fresh(ram, targets[0]); cpu.downcount = -256; denied_unchanged(&cpu, targets[0]);
  CHECK(staticrecomp_try_sdk_leaf(NULL, targets[0], continuation) == 0);
  cpu = fresh(ram, targets[0]); mode = DECLINE;
  { CPUState before = cpu; CHECK(caller(&cpu, targets[0]) == 0); CHECK(calls == 1 && guard_calls == 1); CHECK(memcmp(&cpu, &before, sizeof(cpu)) == 0 && ram[0] == 0); }
  for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
    cpu = fresh(ram, targets[i]);
    CPUState expected = cpu; expected.ram = chassis_ram; chassis_ram[0] = 0;
    CHECK(leaf(&expected) == 1); expected.downcount--; expected.gpr[4]++;
    calls = 0;
    CHECK(caller(&cpu, targets[i]) == 1 && calls == 1 && guard_calls == 2);
    expected.ram = ram;
    CHECK(memcmp(&cpu, &expected, sizeof(cpu)) == 0 && ram[0] == chassis_ram[0]);
  }
  cpu = fresh(ram, targets[0]); mode = INSTALL_HOOK;
  CHECK(caller(&cpu, targets[0]) == 2 && guard_calls == 2 && cpu.gpr[4] == 0 && cpu.pc == continuation);
  cpu = fresh(ram, targets[0]); mode = EXCEPTION;
  CHECK(caller(&cpu, targets[0]) == 2 && cpu.exception && cpu.pc == 0x80000700u && cpu.gpr[4] == 0);
  cpu = fresh(ram, targets[0]); mode = OTHER_PC;
  CHECK(caller(&cpu, targets[0]) == 2 && cpu.pc == 0x80002000u && cpu.gpr[4] == 0);
  cpu = fresh(ram, targets[0]); mode = SPEND_BUDGET;
  CHECK(caller(&cpu, targets[0]) == 2 && cpu.downcount == -257 && cpu.gpr[4] == 0);
  cpu = fresh(ram, targets[0]); cpu.downcount = -255;
  CHECK(caller(&cpu, targets[0]) == 2 && calls == 1 && cpu.downcount == -256 && cpu.gpr[4] == 0);
  cpu = fresh(ram, targets[0]); staticrecomp_set_sdk_guard_v1(NULL, NULL); denied_unchanged(&cpu, targets[0]);
  puts("SDK leaf guard tests passed");
  return 0;
}
