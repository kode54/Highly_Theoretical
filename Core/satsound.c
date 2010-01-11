/////////////////////////////////////////////////////////////////////////////
//
// satsound - Saturn sound system emulation
//
/////////////////////////////////////////////////////////////////////////////

#ifndef EMU_COMPILE
#error "Hi I forgot to set EMU_COMPILE"
#endif

#include "satsound.h"

#include "Starscream/starcpu.h"
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
  uint32 sound_samples_remaining;
  uint32 cycles_ahead_of_sound;
  sint32 cycles_executed;
};

// bytes to either side of RAM to prevent branch overflow problems
#define RAMSLOP (0x9000)

#define SATSOUNDSTATE ((struct SATSOUND_STATE*)(state))
#define MAPS        ((void*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_maps)))
#define SCPUSTATE   ((void*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_scpu)))
#define YAMSTATE    ((void*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_yam)))
#define RAMBYTEPTR (((uint8*)(((char*)(SATSOUNDSTATE))+(SATSOUNDSTATE->offset_to_ram)))+RAMSLOP)

extern const uint32 satsound_total_maps_size;

uint32 EMU_CALL satsound_get_state_size(void) {
  uint32 offset = 0;
  offset += sizeof(struct SATSOUND_STATE);
  offset += satsound_total_maps_size;
  offset += s68000_get_state_size();
  offset += yam_get_state_size(1);
  offset += 0x80000 + 2*RAMSLOP;
  return offset;
}

static void recompute_and_set_memory_maps(struct SATSOUND_STATE *state);

/////////////////////////////////////////////////////////////////////////////
//
// Check to see if this structure has moved, and if so, recompute
//
static void location_check(struct SATSOUND_STATE *state) {
  if(state->myself != state) {
    recompute_and_set_memory_maps(SATSOUNDSTATE);
    yam_setram(YAMSTATE, (uint32*)(RAMBYTEPTR), 0x80000, EMU_ENDIAN_XOR(1) ^ 1, 0);
    state->myself = state;
  }
}

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
  SATSOUNDSTATE->offset_to_maps      = offset; offset += satsound_total_maps_size;
  SATSOUNDSTATE->offset_to_scpu      = offset; offset += s68000_get_state_size();
  SATSOUNDSTATE->offset_to_yam       = offset; offset += yam_get_state_size(1);
  SATSOUNDSTATE->offset_to_ram       = offset; offset += 0x80000 + 2*RAMSLOP;

  //
  // Take care of substructures
  //
  memset(RAMBYTEPTR-RAMSLOP, 0xFF, RAMSLOP);
  memset(RAMBYTEPTR        , 0x00, 0x80000);
  memset(RAMBYTEPTR+0x80000, 0xFF, RAMSLOP);
  s68000_clear_state(SCPUSTATE);
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
void* EMU_CALL satsound_get_scpu_state(void *state) { return SCPUSTATE; }
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

  s68000_reset(SCPUSTATE);
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
  odometer = s68000_read_odometer(SCPUSTATE);
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
  SATSOUNDSTATE->scpu_odometer_checkpoint = s68000_read_odometer(SCPUSTATE);
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

  //
  // Execution loop
  //
  while(SATSOUNDSTATE->cycles_executed < cycles) {
    uint32 r;
    uint32 remain = cycles - SATSOUNDSTATE->cycles_executed;
    uint32 ci = cycles_until_next_interrupt(SATSOUNDSTATE);
    if(remain > ci) { remain = ci; }
    if(remain > 0x1000000) { remain = 0x1000000; }

    if((SATSOUNDSTATE->yam_prev_int) != (*yamintptr)) {
      SATSOUNDSTATE->yam_prev_int = (*yamintptr);
      if(*yamintptr) {
//printf("interrupt %d\n",(int)(*yamintptr));
        s68000_interrupt(
          SCPUSTATE,
          ((-1)*256) + ((SATSOUNDSTATE->yam_prev_int)&7)
        );
      }
    }
//printf("executing remain=%d\n",remain);
    r = s68000_execute(SCPUSTATE, remain);
    if(r != 0x80000000) {
      error = -1; break;
    }
    satsound_advancesync(SATSOUNDSTATE);
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
  return s68000_getreg(SCPUSTATE, STARSCREAM_REG_PC);
}

/////////////////////////////////////////////////////////////////////////////
