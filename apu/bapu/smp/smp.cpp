#ifdef DEBUGGER
#include "../../../snes9x.h"
#include "../../../debug.h"
char tmp[1024];
#endif

#include "../snes/snes.hpp"

#define SMP_CPP
namespace SNES {

#ifdef DEBUGGER
#include "debugger/disassembler.cpp"
#endif

SMP smp;

#include "algorithms.cpp"
#include "core.cpp"
#include "iplrom.cpp"
#include "memory.cpp"
#include "timing.cpp"

void SMP::enter() {
#ifdef _EE
  ee_enter();
#else
  while(clock < 0) op_step();
#endif
}

#ifdef _EE
int SMP::ee_run_ahead(int budget, int *write_before) {
  uint32 ports, now;
  memcpy(&ports, apuram + 0xf4, 4);
  int spent = 0;
  *write_before = -1;
  while(spent < budget) {
    int c0 = clock;
    op_step();
    int step = clock - c0;
    memcpy(&now, apuram + 0xf4, 4);
    if(now != ports) {
      *write_before = spent;
      return spent + step;
    }
    spent += step;
  }
  return spent;
}
#endif

void SMP::power() {
  Processor::clock = 0;

  timer0.target = 0;
  timer1.target = 0;
  timer2.target = 0;

  reset();
}

void SMP::reset() {
  for(unsigned n = 0x0000; n <= 0xffff; n++) apuram[n] = 0x00;

  opcode_number = 0;
  opcode_cycle = 0;

  regs.pc = 0xffc0;
  regs.sp = 0xef;
  regs.B.a = 0x00;
  regs.x = 0x00;
  regs.B.y = 0x00;
  regs.p = 0x02;

  //$00f1
  status.iplrom_enable = true;

  //$00f2
  status.dsp_addr = 0x00;

  //$00f8,$00f9
  status.ram00f8 = 0x00;
  status.ram00f9 = 0x00;

  //timers
  timer0.enable = timer1.enable = timer2.enable = false;
  timer0.stage1_ticks = timer1.stage1_ticks = timer2.stage1_ticks = 0;
  timer0.stage2_ticks = timer1.stage2_ticks = timer2.stage2_ticks = 0;
  timer0.stage3_ticks = timer1.stage3_ticks = timer2.stage3_ticks = 0;
#ifdef _EE
  timer_pending = 0;
  ee_idle_last.valid = false;
#endif
}

SMP::SMP() {
  apuram = new uint8[64 * 1024];
#ifdef _EE
  timer_pending = 0;
  ee_cycles = 0;
  ee_idle_last.valid = false;
  ee_idle_skips = ee_idle_skipped_cycles = 0;
#endif
}

SMP::~SMP() {
	delete[] apuram;
}

}
