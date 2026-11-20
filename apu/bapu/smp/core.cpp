void SMP::tick() {
#ifdef _EE
  timer_pending++;
  ee_cycles++;
#else
  timer0.tick();
  timer1.tick();
  timer2.tick();
#endif

  clock++;
  dsp.clock++;
}

void SMP::tick(unsigned clocks) {
#ifdef _EE
  timer_pending += clocks;
  ee_cycles += clocks;
#else
  timer0.tick(clocks);
  timer1.tick(clocks);
  timer2.tick(clocks);
#endif

  clock += clocks;
  dsp.clock += clocks;
}

void SMP::op_io() {
  tick();
}

void SMP::op_io(unsigned clocks) {
  tick(clocks);
}

uint8 SMP::op_read(uint16 addr) {
  tick();
  if((addr & 0xfff0) == 0x00f0) return mmio_read(addr);
  if(addr >= 0xffc0 && status.iplrom_enable) return iplrom[addr & 0x3f];
  return apuram[addr];
}

void SMP::op_write(uint16 addr, uint8 data) {
  tick();
  if((addr & 0xfff0) == 0x00f0) mmio_write(addr, data);
  apuram[addr] = data;  //all writes go to RAM, even MMIO writes
}

uint8 SMP::op_readstack()
{
  tick();
  return apuram[0x0100 | ++regs.sp];
}

void SMP::op_writestack(uint8 data)
{
  tick();
  apuram[0x0100 | regs.sp--] = data;
}

void SMP::op_step() {
  #define op_readpc() op_read(regs.pc++)
  #define op_readdp(addr) op_read((regs.p.p << 8) + ((addr) & 0xff))
  #define op_writedp(addr, data) op_write((regs.p.p << 8) + ((addr) & 0xff), data)
  #define op_readaddr(addr) op_read(addr)
  #define op_writeaddr(addr, data) op_write(addr, data)

  if(opcode_cycle == 0)
  {
#ifdef DEBUGGER
    if (Settings.TraceSMP)
    {
      disassemble_opcode(tmp, regs.pc);
      S9xTraceMessage (tmp);
    }
#endif
    opcode_number = op_readpc();
  }

  switch(opcode_number) {
    #include "core/oppseudo_misc.cpp"
    #include "core/oppseudo_mov.cpp"
    #include "core/oppseudo_pc.cpp"
    #include "core/oppseudo_read.cpp"
    #include "core/oppseudo_rmw.cpp"
  }
}

#ifdef _EE
/* PS2 port: the interpreter loop the port runs (SMP::enter).
 *
 * op_step() above pays, for every bus cycle of every opcode, a call to
 * op_read()/op_write() and four counter updates in tick() - a fifth of
 * the emulator's time on the EE. This is the same interpreter - the very
 * same opcode bodies, included a second time - with the bus accesses
 * inline and the cycles counted in a local, handed to tick() in one go:
 *   - at the end of each step, before the "slice done?" test;
 *   - before anything that can see the time: an access to $f0-$ff (timers
 *     are synced from the cycle count, a DSP access catches the DSP up to
 *     it), and the idle-loop check.
 * Nothing else observes time within a step, so the result is the same
 * state at every point where op_step() would have returned; the host
 * oracle's APU checksums and its --smp-selftest hold it to that. */
uint8 SMP::ee_read_slow(uint16 addr) {
  if((addr & 0xfff0) == 0x00f0) return mmio_read(addr);
  if(addr >= 0xffc0 && status.iplrom_enable) return iplrom[addr & 0x3f];
  return apuram[addr];
}

#undef op_readpc
#undef op_readdp
#undef op_writedp
#undef op_readaddr
#undef op_writeaddr

void SMP::ee_enter() {
  if(clock >= 0) return;

  uint8 *const ram = apuram;
  unsigned cyc = 0;  /* cycles run, not yet given to tick() */

  /* Plain RAM: everything but $00f0-$00ff (I/O) and $ffc0- (IPL ROM, when
   * enabled). */
  auto rd8 = [&](uint16 addr) __attribute__((always_inline)) -> uint8 {
    cyc++;
    if(__builtin_expect((unsigned)(addr - 0x00f0) < 0x10 || addr >= 0xffc0, 0)) {
      tick(cyc);
      cyc = 0;
      return ee_read_slow(addr);
    }
    return ram[addr];
  };
  auto wr8 = [&](uint16 addr, uint8 data) __attribute__((always_inline)) {
    cyc++;
    if(__builtin_expect((unsigned)(addr - 0x00f0) < 0x10, 0)) {
      tick(cyc);
      cyc = 0;
      mmio_write(addr, data);
    }
    ram[addr] = data;  //all writes go to RAM, even MMIO writes
  };
  auto rdstack = [&]() __attribute__((always_inline)) -> uint8 {
    cyc++;
    return ram[0x0100 | ++regs.sp];
  };
  auto wrstack = [&](uint8 data) __attribute__((always_inline)) {
    cyc++;
    ram[0x0100 | regs.sp--] = data;
  };
  struct Io {
    unsigned &cyc;
    alwaysinline void operator()() { cyc++; }
    alwaysinline void operator()(unsigned clocks) { cyc += clocks; }
  } io = { cyc };

  #define op_readpc() rd8(regs.pc++)
  #define op_readdp(addr) rd8((regs.p.p << 8) + ((addr) & 0xff))
  #define op_writedp(addr, data) wr8((regs.p.p << 8) + ((addr) & 0xff), data)
  #define op_readaddr(addr) rd8(addr)
  #define op_writeaddr(addr, data) wr8(addr, data)
  #define op_readstack() rdstack()
  #define op_writestack(data) wrstack(data)
  #define op_io io
  #define ee_idle_check(d) do { tick(cyc); cyc = 0; (ee_idle_check)(d); } while(0)

  do {
    if(opcode_cycle == 0) opcode_number = op_readpc();

    switch(opcode_number) {
      #include "core/oppseudo_misc.cpp"
      #include "core/oppseudo_mov.cpp"
      #include "core/oppseudo_pc.cpp"
      #include "core/oppseudo_read.cpp"
      #include "core/oppseudo_rmw.cpp"
    }
  } while((int)(clock + cyc) < 0);

  tick(cyc);

  #undef op_readpc
  #undef op_readdp
  #undef op_writedp
  #undef op_readaddr
  #undef op_writeaddr
  #undef op_readstack
  #undef op_writestack
  #undef op_io
  #undef ee_idle_check
}
#endif
