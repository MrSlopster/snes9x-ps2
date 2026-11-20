template<unsigned cycle_frequency>
void SMP::Timer<cycle_frequency>::tick() {
  if(++stage1_ticks < cycle_frequency) return;

  stage1_ticks = 0;
  if(enable == false) return;

  if(++stage2_ticks != target) return;

  stage2_ticks = 0;
  stage3_ticks = (stage3_ticks + 1) & 15;
}

template<unsigned cycle_frequency>
void SMP::Timer<cycle_frequency>::tick(unsigned clocks) {
  stage1_ticks += clocks;
  if(stage1_ticks < cycle_frequency) return;

  stage1_ticks -= cycle_frequency;
  if(enable == false) return;

  if(++stage2_ticks != target) return;

  stage2_ticks = 0;
  stage3_ticks = (stage3_ticks + 1) & 15;
}

#ifdef _EE
/* Same result as 'clocks' calls to tick(): stage1 counts modulo the
 * frequency; each stage1 rollover pre-increments the uint8 stage2, which
 * hits 'target' after d = (target - stage2) mod 256 steps (256 when d is
 * 0) and then every target steps (256 for target 0), bumping stage3. */
template<unsigned cycle_frequency>
void SMP::Timer<cycle_frequency>::advance(unsigned clocks) {
  unsigned s1 = stage1_ticks + clocks;
  unsigned steps = s1 / cycle_frequency;
  stage1_ticks = s1 % cycle_frequency;
  if(steps == 0 || enable == false) return;

  unsigned d = (uint8)(target - stage2_ticks);
  if(d == 0) d = 256;
  if(steps < d) {
    stage2_ticks = stage2_ticks + steps;
    return;
  }
  unsigned period = target ? target : 256;
  unsigned rest = steps - d;
  stage2_ticks = rest % period;
  stage3_ticks = (stage3_ticks + 1 + rest / period) & 15;
}

void SMP::timers_sync() {
  if(timer_pending == 0) return;
  timer0.advance(timer_pending);
  timer1.advance(timer_pending);
  timer2.advance(timer_pending);
  timer_pending = 0;
}
#endif

#if defined(_EE) && !defined(SMP_TIMERS_ONLY) /* host timer test builds only the timer math */
/* Cycles until this timer's stage3 counter next increments (timers must
 * be synced); ~0u if it is disabled. Same stage arithmetic as advance(). */
template<unsigned cycle_frequency>
static unsigned ee_cycles_to_tick(const SMP::Timer<cycle_frequency> &t) {
  if(t.enable == false) return ~0u;
  unsigned d = (uint8)(t.target - t.stage2_ticks);
  if(d == 0) d = 256;
  return (cycle_frequency - t.stage1_ticks) + (d - 1) * cycle_frequency;
}

void SMP::ee_idle_check(int displacement) {
  if(displacement >= 0 || displacement < -12) return;

  /* regs.pc is already the branch target; the 2-byte branch ends at 'after'. */
  uint16 target = regs.pc;
  uint16 after = target - displacement;
  uint8 psw = (unsigned)regs.p;

  bool same = ee_idle_last.valid && ee_idle_last.pc == target &&
              ee_idle_last.a == regs.B.a && ee_idle_last.x == regs.x &&
              ee_idle_last.y == regs.B.y && ee_idle_last.sp == regs.sp &&
              ee_idle_last.psw == psw;
  unsigned c = ee_cycles - ee_idle_last.cycles;

  ee_idle_last.valid = true;
  ee_idle_last.pc = target;
  ee_idle_last.a = regs.B.a;
  ee_idle_last.x = regs.x;
  ee_idle_last.y = regs.B.y;
  ee_idle_last.sp = regs.sp;
  ee_idle_last.psw = psw;
  ee_idle_last.cycles = ee_cycles;

  if(!same || c == 0 || c > 64 || clock >= 0) return;
  if(regs.p.p) return;                     /* direct page must be $00xx */
  if(target < 0x0100 || after > 0xffc0) return; /* keep clear of I/O and IPL ROM */

  /* Decode the body [target, after - 2): loads/compares only, operands
   * limited to immediates, input ports and timer counters. The bytes are
   * read fresh every time: SPC code lives in RAM. */
  bool reads_timer[3] = { false, false, false };
  unsigned cost = 4;  /* the taken branch itself */
  uint16 pos = target, end = after - 2;
  while(pos != end) {
    if((uint16)(end - pos) < 1) return;
    uint8 op = apuram[pos];
    unsigned len;
    bool mem_operand = false;
    switch(op) {
      case 0x00: len = 1; cost += 2; break;                            /* NOP */
      case 0xe8: case 0x8d: case 0xcd:                                 /* MOV A/Y/X,#imm */
      case 0x68: case 0xc8: case 0xad: len = 2; cost += 2; break;      /* CMP A/X/Y,#imm */
      case 0xe4: case 0xeb: case 0xf8:                                 /* MOV A/Y/X,dp */
      case 0x64: case 0x3e: case 0x7e: len = 2; cost += 3; mem_operand = true; break; /* CMP A/X/Y,dp */
      case 0xe5: case 0xec: case 0xe9:                                 /* MOV A/Y/X,!abs */
      case 0x65: case 0x1e: case 0x5e: len = 3; cost += 4; mem_operand = true; break; /* CMP A/X/Y,!abs */
      default: return;
    }
    if((uint16)(end - pos) < len) return;
    if(mem_operand) {
      /* dp (page 0, checked above) or !abs with a zero high byte */
      uint8 addr = apuram[(uint16)(pos + 1)];
      if(len == 3 && apuram[(uint16)(pos + 2)] != 0) return;
      if(addr >= 0xfd) reads_timer[addr - 0xfd] = true;
      else if(addr < 0xf4 || addr > 0xf7) return;
    }
    pos += len;
  }

  /* The gap between the two branch hits must be exactly one pass of this
   * body (SPC700 cycle counts): identical registers alone do not prove the
   * hits were consecutive passes - the driver may have left the loop, done
   * a little work and come back. */
  if(c != cost) return;

  /* Whole passes that fit in this slice, leaving the last one to run. */
  int k = (-clock) / (int)c - 1;
  if(k <= 0) return;

  if(reads_timer[0] || reads_timer[1] || reads_timer[2]) {
    timers_sync();
    /* A tick that landed after this pass's read is already waiting in the
     * counter: the very next pass will see it. */
    if((reads_timer[0] && timer0.stage3_ticks) || (reads_timer[1] && timer1.stage3_ticks) ||
       (reads_timer[2] && timer2.stage3_ticks)) return;
    unsigned until = ~0u;
    if(reads_timer[0]) { unsigned t = ee_cycles_to_tick(timer0); if(t < until) until = t; }
    if(reads_timer[1]) { unsigned t = ee_cycles_to_tick(timer1); if(t < until) until = t; }
    if(reads_timer[2]) { unsigned t = ee_cycles_to_tick(timer2); if(t < until) until = t; }
    if(until != ~0u) {
      int kt = (int)(until / c) - 1;
      if(kt < k) k = kt;
    }
    if(k <= 0) return;
  }

  tick((unsigned)k * c);
  ee_idle_skips++;
  ee_idle_skipped_cycles += (unsigned)k * c;
  ee_idle_last.cycles = ee_cycles;
}
#endif
