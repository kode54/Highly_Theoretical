/////////////////////////////////////////////////////////////////////////////
//
// satsound - Saturn sound system emulation
//
/////////////////////////////////////////////////////////////////////////////

#ifndef EMU_COMPILE
#error "Hi I forgot to set EMU_COMPILE"
#endif

#include "satsound.h"

// #define USE_STARSCREAM
#ifdef USE_STARSCREAM
#include "Starscream/starcpu.h"
#else
#include "c68k/c68k.h"
#endif

#include "yam.h"

/////////////////////////////////////////////////////////////////////////////
//
// Static information
//
sint32 EMU_CALL satsound_init(void) { return 0; }

#define CYCLES_PER_SAMPLE (256)

/////////////////////////////////////////////////////////////////////////////
//
// State information
//
struct SATSOUND_STATE {
  struct SATSOUND_STATE *myself; // Pointer used to check location invariance

  uint32 offset_to_maps;
  uint32 offset_to_scpu;
  uint32 offset_to_yam;
  uint32 offset_to_ram;

  uint8 yam_prev_int;
//  uint8 scpu_is_executing;

  uint32 scpu_odometer_checkpoint;
  uint32 scpu_odometer_save;
  uint32 sound_samples_remaining;
  uint32 cycles_ahead_of_sound;
  sint32 cycles_executed;
};

// bytes to either side of RAM to prevent branch overflow problems
#define RAMSLOP (0x9000)

#define SATSOUNDSTATE ((struct SATSOUND_STATE*)(state))
#define MAPS        ((void*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_maps)))
#ifdef USE_STARSCREAM
#define SCPUSTATE   ((void*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_scpu)))
#else
#define SCPUSTATE   ((c68k_struc*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_scpu)))
#endif
#define YAMSTATE    ((void*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_yam)))
#define RAMBYTEPTR (((uint8*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_ram)))+RAMSLOP)

#ifdef USE_STARSCREAM
extern const uint32 satsound_total_maps_size;
#endif

uint32 EMU_CALL satsound_get_state_size(void) {
  uint32 offset = 0;
  offset += sizeof(struct SATSOUND_STATE);
#ifdef USE_STARSCREAM
  offset += satsound_total_maps_size;
  offset += s68000_get_state_size();
#else
  offset += sizeof(c68k_struc);
#endif
  offset += yam_get_state_size(1);
  offset += 0x80000 + 2*RAMSLOP;
  return offset;
}

#ifdef USE_STARSCREAM
static void recompute_and_set_memory_maps(struct SATSOUND_STATE *state);
#endif

/////////////////////////////////////////////////////////////////////////////
//
// Check to see if this structure has moved, and if so, recompute
//
static void location_check(struct SATSOUND_STATE *state) {
  if(state->myself != state) {
#ifdef USE_STARSCREAM
    recompute_and_set_memory_maps(SATSOUNDSTATE);
#else
    C68k_Set_Fetch(SCPUSTATE, 0x00000, 0x7FFFF, RAMBYTEPTR);
#endif
    yam_setram(YAMSTATE, (uint32*)(RAMBYTEPTR), 0x80000, EMU_ENDIAN_XOR(1) ^ 1, 0);
    state->myself = state;
  }
}

#ifndef USE_STARSCREAM
/////////////////////////////////////////////////////////////////////////////
//
// CPU access callbacks
//
static void satsound_advancesync(struct SATSOUND_STATE *state);

u32 FASTCALL satsound_cb_readb(void *state, const u32 address)
{
	if (address < (512*1024)) return RAMBYTEPTR[address^EMU_ENDIAN_XOR(1)^1];

	if (address >= 0x100000 && address < 0x100c00)
	{
		int shift = ((address & 1) ^ 1) * 8;
		satsound_advancesync(SATSOUNDSTATE);
		return (yam_scsp_load_reg(YAMSTATE, address & 0xFFE, 0xFF << shift) >> shift) & 0xFF;
	}

	return 0;
}

u32 FASTCALL satsound_cb_readw(void *state, const u32 address)
{
	if (address < (512*1024)) return ((uint16*)(RAMBYTEPTR))[address/2];

	if (address >= 0x100000 && address < 0x100c00)
	{
		satsound_advancesync(SATSOUNDSTATE);
		return yam_scsp_load_reg(YAMSTATE, address & 0xFFE, 0xFFFF);
	}

	return 0;
}

void FASTCALL satsound_cb_writeb(void *state, const u32 address, u32 data)
{
	if (address < (512*1024))
	{
		RAMBYTEPTR[address^EMU_ENDIAN_XOR(1)^1] = data;
		return;
	}

	if (address >= 0x100000 && address < 0x100c00)
	{
		uint8 breakcpu = 0;
		int shift = ((address & 1) ^ 1) * 8;
		satsound_advancesync(SATSOUNDSTATE);
		//printf("satsound_yam_writebyte(%08X,%08X)\n",address,data);
		yam_scsp_store_reg(
			YAMSTATE,
			address & 0xFFE,
			(data & 0xFF) << shift,
			0xFF << shift,
			&breakcpu
			);
		if(breakcpu) C68k_Release_Cycle(SCPUSTATE);
		return;
	}
}

void FASTCALL satsound_cb_writew(void *state, const u32 address, u32 data)
{
	if (address < (512*1024))
	{
		((uint16*)(RAMBYTEPTR))[address/2] = data;
		return;
	}

	if (address >= 0x100000 && address < 0x100c00)
	{
		uint8 breakcpu = 0;
		satsound_advancesync(SATSOUNDSTATE);
		//printf("satsound_yam_writeword(%08X,%08X)\n",address,data);
		yam_scsp_store_reg(
			YAMSTATE,
			address & 0xFFE,
			data & 0xFFFF,
			0xFFFF,
			&breakcpu
			);
		if(breakcpu) C68k_Release_Cycle(SCPUSTATE);
		return;
	}
}
#endif

/////////////////////////////////////////////////////////////////////////////
//
// Clear state
//
void EMU_CALL satsound_clear_state(void *state) {
  uint32 offset;

  // Clear local struct
  memset(state, 0, sizeof(struct SATSOUND_STATE));

  // Set up offsets
  offset = sizeof(struct SATSOUND_STATE);
#ifdef USE_STARSCREAM
  SATSOUNDSTATE->offset_to_maps      = offset; offset += satsound_total_maps_size;
  SATSOUNDSTATE->offset_to_scpu      = offset; offset += s68000_get_state_size();
#else
  SATSOUNDSTATE->offset_to_maps      = offset;
  SATSOUNDSTATE->offset_to_scpu      = offset; offset += sizeof(c68k_struc);
#endif
  SATSOUNDSTATE->offset_to_yam       = offset; offset += yam_get_state_size(1);
  SATSOUNDSTATE->offset_to_ram       = offset; offset += 0x80000 + 2*RAMSLOP;

  //
  // Take care of substructures
  //
  memset(RAMBYTEPTR-RAMSLOP, 0xFF, RAMSLOP);
  memset(RAMBYTEPTR        , 0x00, 0x80000);
  memset(RAMBYTEPTR+0x80000, 0xFF, RAMSLOP);
#ifdef USE_STARSCREAM
  s68000_clear_state(SCPUSTATE);
#else
  C68k_Init(SCPUSTATE, NULL);

  C68k_Set_Callback_Param(SCPUSTATE, state);
  C68k_Set_Fetch(SCPUSTATE, 0x00000, 0x7FFFF, RAMBYTEPTR);
  C68k_Set_ReadB(SCPUSTATE, satsound_cb_readb);
  C68k_Set_ReadW(SCPUSTATE, satsound_cb_readw);
  C68k_Set_WriteB(SCPUSTATE, satsound_cb_writeb);
  C68k_Set_WriteW(SCPUSTATE, satsound_cb_writew);
#endif
  yam_clear_state(YAMSTATE, 1);
  // No idea what to initialize the interrupt system to, so leave it alone

  //
  // Compute all location-dependent pointers
  //
  location_check(SATSOUNDSTATE);

  // Done
}

/////////////////////////////////////////////////////////////////////////////
//
// Obtain substates
//
void* EMU_CALL satsound_get_scpu_state(void *state) { return (void*)SCPUSTATE; }
void* EMU_CALL satsound_get_yam_state(void *state) { return YAMSTATE; }

/////////////////////////////////////////////////////////////////////////////
//
// Upload data to RAM, no side effects
//
void EMU_CALL satsound_upload_to_ram(
  void *state,
  uint32 address,
  void *src,
  uint32 len
) {
  uint32 i;
  for(i = 0; i < len; i++) {
    (RAMBYTEPTR)[((address+i)^(EMU_ENDIAN_XOR(1)^1))&0x7FFFF] =
      ((uint8*)src)[i];
  }

#ifdef USE_STARSCREAM
  s68000_reset(SCPUSTATE);
#else
  C68k_Reset(SCPUSTATE);
#endif
}

/////////////////////////////////////////////////////////////////////////////
//
// Sync Yamaha emulation with satsound
//
static void sync_sound(struct SATSOUND_STATE *state) {
  if(state->cycles_ahead_of_sound >= CYCLES_PER_SAMPLE) {
    uint32 samples = (state->cycles_ahead_of_sound) / CYCLES_PER_SAMPLE;
    //
    // Avoid overflowing the number of samples remaining
    //
    if(samples > state->sound_samples_remaining) {
      samples = state->sound_samples_remaining;
    }
    if(samples > 0) {
      yam_advance(YAMSTATE, samples);
      state->cycles_ahead_of_sound -= CYCLES_PER_SAMPLE * samples;
      state->sound_samples_remaining -= samples;
    }
  }
}

/////////////////////////////////////////////////////////////////////////////
//
// Advance hardware activity to match progress by SCPU
//
static void satsound_advancesync(struct SATSOUND_STATE *state) {
  uint32 odometer, elapse;
  //
  // Get the number of elapsed cycles
  //
#ifdef USE_STARSCREAM
  odometer = s68000_read_odometer(SCPUSTATE);
#else
  odometer = C68k_Get_CycleDone(SCPUSTATE);
  if(odometer == ~0) odometer = state->scpu_odometer_save;
#endif
  elapse = odometer - (state->scpu_odometer_checkpoint);
  state->scpu_odometer_checkpoint = odometer;
  //
  // Update cycles executed
  //
  state->cycles_executed += elapse;
  state->cycles_ahead_of_sound += elapse;
  //
  // Synchronize the sound part
  //
  sync_sound(SATSOUNDSTATE);
}

#ifdef USE_STARSCREAM
/////////////////////////////////////////////////////////////////////////////
//
// Starscream I/O callbacks
//
static unsigned STARSCREAM_CALL satsound_yam_readbyte(
  void *state,
  unsigned address
) {
  int shift = ((address & 1) ^ 1) * 8;
  satsound_advancesync(SATSOUNDSTATE);
  return (yam_scsp_load_reg(YAMSTATE, address & 0xFFE, 0xFF << shift) >> shift) & 0xFF;
}

static unsigned STARSCREAM_CALL satsound_yam_readword(
  void *state,
  unsigned address
) {
  satsound_advancesync(SATSOUNDSTATE);
  return yam_scsp_load_reg(YAMSTATE, address & 0xFFE, 0xFFFF) & 0xFFFF;
}

static void STARSCREAM_CALL satsound_yam_writebyte(
  void *state,
  unsigned address,
  unsigned data
) {
  uint8 breakcpu = 0;
  int shift = ((address & 1) ^ 1) * 8;
  satsound_advancesync(SATSOUNDSTATE);
//printf("satsound_yam_writebyte(%08X,%08X)\n",address,data);
  yam_scsp_store_reg(
    YAMSTATE,
    address & 0xFFE,
    (data & 0xFF) << shift,
    0xFF << shift,
    &breakcpu
  );
  if(breakcpu) s68000_break(SCPUSTATE);
}

static void STARSCREAM_CALL satsound_yam_writeword(
  void *state,
  unsigned address,
  unsigned data
) {
  uint8 breakcpu = 0;
  satsound_advancesync(SATSOUNDSTATE);
//printf("satsound_yam_writeword(%08X,%08X)\n",address,data);
  yam_scsp_store_reg(
    YAMSTATE,
    address & 0xFFE,
    data & 0xFFFF,
    0xFFFF,
    &breakcpu
  );
  if(breakcpu) s68000_break(SCPUSTATE);
}

/////////////////////////////////////////////////////////////////////////////
//
// Static memory map structures
//

static const struct STARSCREAM_PROGRAMREGION satsound_map_fetch[] = {
  { 0x000000, 0x07FFFF, 0 },
  { -1, -1, NULL }
};

static const struct STARSCREAM_DATAREGION satsound_map_readbyte[] = {
  { 0x000000, 0x07FFFF, NULL, NULL },
  { 0x100000, 0x100FFF, satsound_yam_readbyte, NULL },
  { -1, -1, NULL, NULL }
};

static const struct STARSCREAM_DATAREGION satsound_map_readword[] = {
  { 0x000000, 0x07FFFF, NULL, NULL },
  { 0x100000, 0x100FFF, satsound_yam_readword, NULL },
  { -1, -1, NULL, NULL }
};

static const struct STARSCREAM_DATAREGION satsound_map_writebyte[] = {
  { 0x000000, 0x07FFFF, NULL, NULL },
  { 0x100000, 0x100FFF, satsound_yam_writebyte, NULL },
  { -1, -1, NULL, NULL }
};

static const struct STARSCREAM_DATAREGION satsound_map_writeword[] = {
  { 0x000000, 0x07FFFF, NULL, NULL },
  { 0x100000, 0x100FFF, satsound_yam_writeword, NULL },
  { -1, -1, NULL, NULL }
};

static void *computemap_program(
  struct SATSOUND_STATE *state,
  struct STARSCREAM_PROGRAMREGION *dst,
  const struct STARSCREAM_PROGRAMREGION *src
) {
  int i;
  for(i = 0;; i++) {
    memcpy(dst + i, src + i, sizeof(struct STARSCREAM_PROGRAMREGION));
    if(dst[i].lowaddr == ((uint32)(-1))) { i++; break; }
  }
  dst[0].offset = RAMBYTEPTR;
  return dst + i;
}

static void *computemap_data(
  struct SATSOUND_STATE *state,
  struct STARSCREAM_DATAREGION *dst,
  const struct STARSCREAM_DATAREGION *src
) {
  int i;
  for(i = 0;; i++) {
    memcpy(dst + i, src + i, sizeof(struct STARSCREAM_DATAREGION));
    if(dst[i].lowaddr == ((uint32)(-1))) { i++; break; }
  }
  dst[0].userdata = RAMBYTEPTR;
  dst[1].userdata = state;
  return dst + i;
}

const uint32 satsound_total_maps_size =
  sizeof(satsound_map_fetch) +
  sizeof(satsound_map_readbyte) +
  sizeof(satsound_map_readword) +
  sizeof(satsound_map_writebyte) +
  sizeof(satsound_map_writeword);

/////////////////////////////////////////////////////////////////////////////
//
// Recompute the memory maps AND REGISTER with the SCPU state.
//
static void recompute_and_set_memory_maps(
  struct SATSOUND_STATE *state
) {
  void *mapinfo[10];
  //
  // Create mapinfo for registering with s68000_set_memory_maps
  //
  mapinfo[0] = MAPS;
  mapinfo[1] = computemap_program(state, mapinfo[0], satsound_map_fetch);
  mapinfo[2] = computemap_data   (state, mapinfo[1], satsound_map_readbyte);
  mapinfo[3] = computemap_data   (state, mapinfo[2], satsound_map_readword);
  mapinfo[4] = computemap_data   (state, mapinfo[3], satsound_map_writebyte);
               computemap_data   (state, mapinfo[4], satsound_map_writeword);
  mapinfo[5] = mapinfo[0];
  mapinfo[6] = mapinfo[1];
  mapinfo[7] = mapinfo[2];
  mapinfo[8] = mapinfo[3];
  mapinfo[9] = mapinfo[4];
  //
  // Register memory maps
  //
  s68000_set_memory_maps(SCPUSTATE, mapinfo);
}
#endif

/////////////////////////////////////////////////////////////////////////////
//
// Determine how many cycles until the next interrupt
//
// This is then used as an upper bound for how many cycles can be executed
// before checking for futher interrupts
//
static uint32 cycles_until_next_interrupt(struct SATSOUND_STATE *state) {
  uint32 yamsamples;
  uint32 yamcycles;
  yamsamples = yam_get_min_samples_until_interrupt(YAMSTATE);
  if(yamsamples > 0x10000) { yamsamples = 0x10000; }
  yamcycles = yamsamples * CYCLES_PER_SAMPLE;
  if(yamcycles <= state->cycles_ahead_of_sound) return 1;
  return yamcycles - state->cycles_ahead_of_sound;
}

/////////////////////////////////////////////////////////////////////////////
//
// Executes the given number of cycles or the given number of samples
// (whichever is less)
//
// Sets *sound_samples to the number of samples actually generated,
// which may be ZERO or LESS than the number requested, but never more.
//
// Return value:
// >= 0   The number of cycles actually executed, which may be ZERO, MORE,
//        or LESS than the number requested
// <= -1  Unrecoverable error
//
#if defined(SCSP_LOG) && !defined(USE_STARSCREAM)
unsigned char ** scsp_pc;
unsigned char ** scsp_basepc;
#endif

sint32 EMU_CALL satsound_execute(
  void   *state,
  sint32  cycles,
  sint16 *sound_buf,
  uint32 *sound_samples
) {
  sint32 error = 0;
  uint8 *yamintptr;
  //
  // If we have a bogus cycle count, return error
  //
  if(cycles < 0) { return -1; }
  //
  // Ensure location invariance
  //
  location_check(SATSOUNDSTATE);
  //
  // Cap to sane values to avoid overflow problems
  //
  if(cycles > 0x1000000) { cycles = 0x1000000; }
  if((*sound_samples) > 0x10000) { (*sound_samples) = 0x10000; }
  //
  // Set up the buffer
  //
  yam_beginbuffer(YAMSTATE, sound_buf);
  SATSOUNDSTATE->sound_samples_remaining = *sound_samples;
  //
  // Get the interrupt pending pointer
  //
  yamintptr = yam_get_interrupt_pending_ptr(YAMSTATE);
  //
  // Zero out these counters
  //
  SATSOUNDSTATE->cycles_executed = 0;
#ifdef USE_STARSCREAM
  SATSOUNDSTATE->scpu_odometer_checkpoint = s68000_read_odometer(SCPUSTATE);
#else
  SATSOUNDSTATE->scpu_odometer_checkpoint = 0;
#endif
  //
  // Sync any pending samples from last time
  //
  sync_sound(SATSOUNDSTATE);
  //
  // Cap cycles depending on how many samples we have left to generate
  //
  { sint32 cap = CYCLES_PER_SAMPLE * SATSOUNDSTATE->sound_samples_remaining;
    cap -= SATSOUNDSTATE->cycles_ahead_of_sound;
    if(cap < 0) cap = 0;
    if(cycles > cap) cycles = cap;
  }
  //
  // Reset the 68K if necessary
  //
//  if(!(SATSOUNDSTATE->scpu_is_executing)) {
//    s68000_reset(SCPUSTATE);
//    SATSOUNDSTATE->scpu_is_executing = 1;
//  }

#if defined(SCSP_LOG) && !defined(USE_STARSCREAM)
  scsp_pc = &(SCPUSTATE->PC);
  scsp_basepc = &(SCPUSTATE->BasePC);
#endif

  //
  // Execution loop
  //
  while(SATSOUNDSTATE->cycles_executed < cycles) {
#ifdef USE_STARSCREAM
    uint32 r;
#else
    sint32 r;
#endif
    uint32 remain = cycles - SATSOUNDSTATE->cycles_executed;
    uint32 ci = cycles_until_next_interrupt(SATSOUNDSTATE);
    if(remain > ci) { remain = ci; }
    if(remain > 0x1000000) { remain = 0x1000000; }

    if((SATSOUNDSTATE->yam_prev_int) != (*yamintptr)) {
      SATSOUNDSTATE->yam_prev_int = (*yamintptr);
      if(*yamintptr) {
//printf("interrupt %d\n",(int)(*yamintptr));
#ifdef USE_STARSCREAM
        s68000_interrupt(
          SCPUSTATE,
          ((-1)*256) + ((SATSOUNDSTATE->yam_prev_int)&7)
        );
#else
        C68k_Set_IRQ(
          SCPUSTATE,
          (SATSOUNDSTATE->yam_prev_int)&7
        );
#endif
      }
    }
//printf("executing remain=%d\n",remain);
#ifdef USE_STARSCREAM
    r = s68000_execute(SCPUSTATE, remain);
    if(r != 0x80000000) {
#else
    r = C68k_Exec(SCPUSTATE, remain);
    if(r < 0) {
#endif
      error = -1; break;
    }
#ifndef USE_STARSCREAM
	SATSOUNDSTATE->scpu_odometer_save = r;
#endif
    satsound_advancesync(SATSOUNDSTATE);
#ifndef USE_STARSCREAM
	SATSOUNDSTATE->scpu_odometer_checkpoint = 0;
#endif
  }
  //
  // Flush out actual sound rendering
  //
  yam_flush(YAMSTATE);
  //
  // Adjust outgoing sample count
  //
  (*sound_samples) -= SATSOUNDSTATE->sound_samples_remaining;
  //
  // Done
  //
  if(error) return error;
  return SATSOUNDSTATE->cycles_executed;
}

/////////////////////////////////////////////////////////////////////////////
//
// Get / set memory words with no side effects
//
uint16 EMU_CALL satsound_getword(void *state, uint32 a) {
  return *((uint16*)(RAMBYTEPTR+(a&0x7FFFE)));
}

void EMU_CALL satsound_setword(void *state, uint32 a, uint16 d) {
  *((uint16*)(RAMBYTEPTR+(a&0x7FFFE))) = d;
}

/////////////////////////////////////////////////////////////////////////////
//
// Get the current program counter
//
uint32 EMU_CALL satsound_get_pc(void *state) {
#ifdef USE_STARSCREAM
  return s68000_getreg(SCPUSTATE, STARSCREAM_REG_PC);
#else
  return C68k_Get_PC(SCPUSTATE);
#endif
}

/////////////////////////////////////////////////////////////////////////////
