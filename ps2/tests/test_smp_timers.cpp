/* Host test: the PS2 port's lazy SPC700 timers (bapu smp/timing.cpp,
 * Timer::advance under _EE) must match ticking once per cycle exactly.
 * Compiles the real timing.cpp against a minimal SMP stand-in. Build with
 * -D_EE. */
#include <cstdio>
#include <cstdint>
#include <cstdlib>

typedef uint8_t uint8;

namespace SNES {
struct SMP
{
	template<unsigned cycle_frequency>
	struct Timer
	{
		bool enable;
		uint8 target;
		uint8 stage1_ticks;
		uint8 stage2_ticks;
		uint8 stage3_ticks;

		inline void tick();
		inline void tick(unsigned clocks);
		inline void advance(unsigned clocks);
	};

	Timer<128> timer0;
	Timer<128> timer1;
	Timer<16> timer2;
	unsigned timer_pending;
	void timers_sync();
};
#define SMP_TIMERS_ONLY /* skip the idle-loop code: it needs the full SMP */
#include "../../apu/bapu/smp/timing.cpp"
}

static int failures = 0;

template<unsigned F>
static void check(bool enable, unsigned target, unsigned s1, unsigned s2, unsigned s3, unsigned clocks)
{
	SNES::SMP::Timer<F> a = { enable, (uint8)target, (uint8)s1, (uint8)s2, (uint8)s3 };
	SNES::SMP::Timer<F> b = a;

	for (unsigned i = 0; i < clocks; i++)
		a.tick();
	b.advance(clocks);

	if (a.stage1_ticks != b.stage1_ticks || a.stage2_ticks != b.stage2_ticks || a.stage3_ticks != b.stage3_ticks)
	{
		if (failures < 10)
			printf("FAIL F=%u en=%d tgt=%u s1=%u s2=%u s3=%u n=%u: tick (%u,%u,%u) advance (%u,%u,%u)\n",
			       F, enable, target, s1, s2, s3, clocks,
			       a.stage1_ticks, a.stage2_ticks, a.stage3_ticks,
			       b.stage1_ticks, b.stage2_ticks, b.stage3_ticks);
		failures++;
	}
}

int main()
{
	static const unsigned clock_counts[] = { 0, 1, 2, 15, 16, 17, 127, 128, 129, 255, 256, 1000, 2048, 4095,
						 16384, 32768, 65536, 100000, 1 << 20 };
	unsigned cases = 0;

	for (int en = 0; en < 2; en++)
		for (unsigned tgt = 0; tgt < 256; tgt++)
			for (unsigned s2 = 0; s2 < 256; s2 += (tgt < 8 ? 1 : 7))
				for (unsigned n : clock_counts)
				{
					unsigned s1_128 = (tgt * 37 + s2) % 128, s1_16 = (tgt + s2) % 16;
					check<128>(en, tgt, s1_128, s2, (tgt + n) & 15, n);
					check<16>(en, tgt, s1_16, s2, (s2 + n) & 15, n);
					cases += 2;
				}

	/* Random walks: many small advances == ticks, across syncs. */
	srand(7);
	for (int w = 0; w < 2000; w++)
	{
		SNES::SMP::Timer<16> a = { (bool)(rand() & 1), (uint8)rand(), (uint8)(rand() % 16), (uint8)rand(), (uint8)(rand() & 15) };
		SNES::SMP::Timer<16> b = a;
		for (int step = 0; step < 50; step++)
		{
			unsigned n = rand() % 300;
			for (unsigned i = 0; i < n; i++)
				a.tick();
			b.advance(n);
			cases++;
			if (a.stage1_ticks != b.stage1_ticks || a.stage2_ticks != b.stage2_ticks || a.stage3_ticks != b.stage3_ticks)
			{
				if (failures < 10)
					printf("FAIL random walk %d step %d\n", w, step);
				failures++;
				break;
			}
		}
	}

	printf("%u timer cases\n", cases);
	if (failures)
	{
		printf("RED/FAIL (%d failures)\n", failures);
		return 1;
	}
	printf("GREEN/PASS (0 failures)\n");
	return 0;
}
