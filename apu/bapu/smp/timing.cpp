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
