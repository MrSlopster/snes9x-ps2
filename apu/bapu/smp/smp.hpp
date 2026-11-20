class SMP : public Processor {
public:
  static const uint8 iplrom[64];
  uint8 *apuram;

  unsigned port_read(unsigned port);
  void port_write(unsigned port, unsigned data);

  unsigned mmio_read(unsigned addr);
  void mmio_write(unsigned addr, unsigned data);

  void enter();
#ifdef _EE
  /* PS2 port (handshake fast-forward, apu.cpp): run cycle steps until
   * 'budget' cycles are spent or a step changes an output port
   * ($f4-$f7). Returns the cycles spent; *write_before is the count
   * before the changing step, or -1 if no port changed. */
  int ee_run_ahead(int budget, int *write_before);
#endif
  void power();
  void reset();

  void load_state(uint8 **);
  void save_state(uint8 **);
  void save_spc (uint8 *);
  SMP();
  ~SMP();

//private:
  struct Flags {
    bool n, v, p, b, h, i, z, c;

    alwaysinline operator unsigned() const {
      return (n << 7) | (v << 6) | (p << 5) | (b << 4)
           | (h << 3) | (i << 2) | (z << 1) | (c << 0);
    };

    alwaysinline unsigned operator=(unsigned data) {
      n = data & 0x80; v = data & 0x40; p = data & 0x20; b = data & 0x10;
      h = data & 0x08; i = data & 0x04; z = data & 0x02; c = data & 0x01;
      return data;
    }

    alwaysinline unsigned operator|=(unsigned data) { return operator=(operator unsigned() | data); }
    alwaysinline unsigned operator^=(unsigned data) { return operator=(operator unsigned() ^ data); }
    alwaysinline unsigned operator&=(unsigned data) { return operator=(operator unsigned() & data); }
  };

  unsigned opcode_number;
  unsigned opcode_cycle;

  uint16 rd, wr, dp, sp, ya, bit;

  struct Regs {
    uint16 pc;
    uint8 sp;
    union {
      uint16 ya;
#ifndef __BIG_ENDIAN__
      struct { uint8 a, y; } B;
#else
      struct { uint8 y, a; } B;
#endif
    };
    uint8 x;
    Flags p;
  } regs;

  struct Status {
    //$00f1
    bool iplrom_enable;

    //$00f2
    unsigned dsp_addr;

    //$00f8,$00f9
    unsigned ram00f8;
    unsigned ram00f9;
  } status;

  template<unsigned frequency>
  struct Timer {
    bool enable;
    uint8 target;
    uint8 stage1_ticks;
    uint8 stage2_ticks;
    uint8 stage3_ticks;

    inline void tick();
    inline void tick(unsigned clocks);
#ifdef _EE
    inline void advance(unsigned clocks);
#endif
  };

  Timer<128> timer0;
  Timer<128> timer1;
  Timer< 16> timer2;

#ifdef _EE
  /* PS2 port: timers are advanced lazily. tick() only counts cycles in
   * timer_pending; timers_sync() applies them in closed form before any
   * access that can observe or change timer state ($f1, $fa-$ff,
   * save state). Bit-identical to ticking every cycle. */
  unsigned timer_pending;
  void timers_sync();

  /* PS2 port: SPC700 idle-loop skipping (timing.cpp). Sound drivers spend
   * most of their time polling a timer counter or an input port
   * ("MOV Y,$FD / BEQ"). ee_idle_check() runs after a taken short backward
   * branch; when two consecutive passes left identical registers and the
   * body only loads/compares immediates, $f4-$f7 or $fd-$ff, whole passes
   * are skipped - never past the end of the current slice, and stopping
   * at least one pass before any polled timer's next tick. */
  unsigned ee_cycles;        /* monotonic cycle count (tick) */
  struct {
    bool valid;
    uint16 pc;
    uint8 a, x, y, sp, psw;
    unsigned cycles;
  } ee_idle_last;
  void ee_idle_check(int displacement);
  unsigned ee_idle_skips, ee_idle_skipped_cycles; /* statistics */
#endif

  inline void tick();
  inline void tick(unsigned clocks);
  alwaysinline void op_io();
  alwaysinline void op_io(unsigned clocks);
#ifdef _EE
  /* PS2 port: with these forced inline into every cycle of every opcode,
   * SMP::enter() compiles to ~107 KB - against a 16 KB instruction cache
   * (hardware PMC: ~22k APU I-cache misses per frame). As real functions
   * each access is one call and the interpreter is a fraction of that. */
  __attribute__((noinline)) uint8 op_read(uint16 addr);
  __attribute__((noinline)) void op_write(uint16 addr, uint8 data);
  __attribute__((noinline)) void op_step();
#else
  debugvirtual alwaysinline uint8 op_read(uint16 addr);
  debugvirtual alwaysinline void op_write(uint16 addr, uint8 data);
  debugvirtual alwaysinline void op_step();
#endif
  alwaysinline void op_writestack(uint8 data);
  alwaysinline uint8 op_readstack();
  static const unsigned cycle_count_table[256];
  uint64 cycle_table_cpu[256];
  unsigned cycle_table_dsp[256];
  uint64 cycle_step_cpu;

  inline uint8  op_adc (uint8  x, uint8  y);
  inline uint16 op_addw(uint16 x, uint16 y);
  inline uint8  op_and (uint8  x, uint8  y);
  inline uint8  op_cmp (uint8  x, uint8  y);
  inline uint16 op_cmpw(uint16 x, uint16 y);
  inline uint8  op_eor (uint8  x, uint8  y);
  inline uint8  op_inc (uint8  x);
  inline uint8  op_dec (uint8  x);
  inline uint8  op_or  (uint8  x, uint8  y);
  inline uint8  op_sbc (uint8  x, uint8  y);
  inline uint16 op_subw(uint16 x, uint16 y);
  inline uint8  op_asl (uint8  x);
  inline uint8  op_lsr (uint8  x);
  inline uint8  op_rol (uint8  x);
  inline uint8  op_ror (uint8  x);
#ifdef DEBUGGER
  void disassemble_opcode(char *output, uint16 addr);
  inline uint8 disassemble_read(uint16 addr);
  inline uint16 relb(int8 offset, int op_len);
#endif
};

extern SMP smp;
