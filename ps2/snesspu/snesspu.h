#ifndef SNESSPU_H
#define SNESSPU_H
/* Protocol between the emulator (EE) and snesspu.irx (IOP), which plays
 * the SNES voices on SPU2 hardware voices ("fast sound"). Plain C, shared
 * by both sides and by the host simulator (tools/oracle). All values
 * little-endian, as both CPUs are.
 *
 * The EE sends ADPCM sample data into SPU2 RAM, and a stream of
 * timestamped register events. Event times are in SNES output samples
 * (32 kHz) of emulated time; the IOP plays them back against real time,
 * a fixed delay behind the newest event batch. */

#define SNESSPU_RPC_ID 0x534E5350 /* 'SNSP' */

enum
{
	SNESSPU_CMD_INIT = 1, /* in: none.                        out: u32 version */
	SNESSPU_CMD_UPLOAD,   /* in: snesspu_upload + data.       out: u32 0 / error */
	SNESSPU_CMD_EVENTS,   /* in: snesspu_batch + events.      out: none */
	SNESSPU_CMD_STOP      /* in: none. All voices off, queue dropped. out: none */
};

#define SNESSPU_VERSION 1

/* SPU2 RAM used for samples (byte addresses; the first part of the RAM is
 * the chip's own capture area). Sample starts are passed as 16-byte block
 * numbers in 16 bits, hence the 1 MB limit. */
#define SNESSPU_RAM_BASE 0x6000
#define SNESSPU_RAM_END 0x100000

#define SNESSPU_VOICES 8
#define SNESSPU_UPLOAD_MAX 0x4000  /* data bytes per UPLOAD, multiple of 64 */
#define SNESSPU_BATCH_MAX 1024     /* events per EVENTS */

/* How far behind the newest batch the IOP plays, in 32 kHz samples. The
 * EE emulates a frame in a burst, so this has to cover a frame (533
 * samples) plus scheduling slack. */
#define SNESSPU_DELAY 1100

enum
{
	SNESSPU_EV_KON = 1, /* value: sample start, 16-byte block number */
	SNESSPU_EV_KOFF,
	SNESSPU_EV_VOLL,    /* value: SPU2 voice volume, 15-bit signed */
	SNESSPU_EV_VOLR,
	SNESSPU_EV_PITCH,   /* value: SPU2 pitch (0x1000 = 48 kHz) */
	SNESSPU_EV_MVOLL,   /* value: SPU2 master volume, 15-bit signed */
	SNESSPU_EV_MVOLR,
	SNESSPU_EV_NON,     /* value: voice mask playing noise */
	SNESSPU_EV_PMON,    /* value: voice mask pitch-modulated by the voice below */
	SNESSPU_EV_NOISE,   /* value: SPU2 noise clock (6 bits) */
	SNESSPU_EV_ALL_OFF  /* every voice silenced at once */
};

typedef struct
{
	unsigned int time; /* 32 kHz samples, wraps */
	unsigned char type;
	unsigned char voice;
	unsigned short value;
} snesspu_event;

typedef struct
{
	unsigned int count;
	unsigned int now; /* emulated time when the batch was sent */
} snesspu_batch;

typedef struct
{
	unsigned int addr; /* SPU2 RAM byte address, multiple of 64 */
	unsigned int size; /* bytes, <= SNESSPU_UPLOAD_MAX */
} snesspu_upload;

/* Hardware envelope: full level at once, held; key-off releases linearly
 * in about 10 ms (the SNES takes 8). The envelope proper is applied by
 * the EE through the voice volumes. */
#define SNESSPU_ADSR1 0x00FF
#define SNESSPU_ADSR2 0x1FC8

#endif
