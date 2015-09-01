#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "Core/sega.h"

#include "vgmwrite.h"

#include "../psflib/psflib.h"

inline unsigned get_le32( void const* p )
{
    return  (unsigned) ((unsigned char const*) p) [3] << 24 |
            (unsigned) ((unsigned char const*) p) [2] << 16 |
            (unsigned) ((unsigned char const*) p) [1] <<  8 |
            (unsigned) ((unsigned char const*) p) [0];
}

void set_le32( void* p, unsigned n )
{
    ((unsigned char*) p) [0] = (unsigned char) n;
    ((unsigned char*) p) [1] = (unsigned char) (n >> 8);
    ((unsigned char*) p) [2] = (unsigned char) (n >> 16);
    ((unsigned char*) p) [3] = (unsigned char) (n >> 24);
}

struct sdsf_loader_state
{
    uint8_t * data;
    size_t data_size;
};

int sdsf_loader(void * context, const uint8_t * exe, size_t exe_size,
                const uint8_t * reserved, size_t reserved_size)
{
    if ( exe_size < 4 ) return -1;

    struct sdsf_loader_state * state = ( struct sdsf_loader_state * ) context;

    uint8_t * dst = state->data;

    if ( state->data_size < 4 ) {
        state->data = dst = ( uint8_t * ) malloc( exe_size );
        state->data_size = exe_size;
        memcpy( dst, exe, exe_size );
        return 0;
    }

    uint32_t dst_start = get_le32( dst );
    uint32_t src_start = get_le32( exe );
    dst_start &= 0x7fffff;
    src_start &= 0x7fffff;
    size_t dst_len = state->data_size - 4;
    size_t src_len = exe_size - 4;
    if ( dst_len > 0x800000 ) dst_len = 0x800000;
    if ( src_len > 0x800000 ) src_len = 0x800000;

    if ( src_start < dst_start )
    {
        uint32_t diff = dst_start - src_start;
        state->data_size = dst_len + 4 + diff;
        state->data = dst = ( uint8_t * ) realloc( dst, state->data_size );
        memmove( dst + 4 + diff, dst + 4, dst_len );
        memset( dst + 4, 0, diff );
        dst_len += diff;
        dst_start = src_start;
        set_le32( dst, dst_start );
    }
    if ( ( src_start + src_len ) > ( dst_start + dst_len ) )
    {
        size_t diff = ( src_start + src_len ) - ( dst_start + dst_len );
        state->data_size = dst_len + 4 + diff;
        state->data = dst = ( uint8_t * ) realloc( dst, state->data_size );
        memset( dst + 4 + dst_len, 0, diff );
    }

    memcpy( dst + 4 + ( src_start - dst_start ), exe + 4, src_len );

    return 0;
}

static void * psf_file_fopen( const char * uri )
{
    return fopen( uri, "rb" );
}

static size_t psf_file_fread( void * buffer, size_t size, size_t count, void * handle )
{
    return fread( buffer, size, count, (FILE *) handle );
}

static int psf_file_fseek( void * handle, int64_t offset, int whence )
{
    return fseek( (FILE *) handle, offset, whence );
}

static int psf_file_fclose( void * handle )
{
    fclose( (FILE *) handle );
    return 0;
}

static long psf_file_ftell( void * handle )
{
    return ftell( (FILE *) handle );
}

const psf_file_callbacks psf_file_system =
{
    "\\/|:",
    psf_file_fopen,
    psf_file_fread,
    psf_file_fseek,
    psf_file_fclose,
    psf_file_ftell
};

void rip_vgm(const char * name)
{
  struct sdsf_loader_state state;
  memset(&state, 0, sizeof(state));

  if (psf_load(name, &psf_file_system, 0x11, sdsf_loader, &state, 0, 0, 0) <= 0)
  {
      return;
  }

  vgm_start(name);

  sega_init();

  void *emu = malloc(sega_get_state_size(1));

  if (!emu)
  {
      free(state.data);
      return;
  }

  sega_clear_state(emu, 1);

  sega_enable_dry(emu, 1);
  sega_enable_dsp(emu, 1);

  sega_enable_dsp_dynarec(emu, 0);

  uint32_t start = get_le32(state.data);
  size_t length = state.data_size;
  const size_t max_length = 0x80000;
  if ((start + (length - 4)) > max_length)
      length = max_length - start + 4;
  sega_upload_program(emu, state.data, (uint32_t)length);

  free(state.data);

  for (int i = 0; i < 44100 * 60 * 10; i += 512)
  {
    unsigned int count = 512;
    sega_execute(emu, 0x7fffffff, 0, &count);
  }

  vgm_stop();

  free(emu);
}

int main(int argc, const char ** argv)
{
  for (int i = 1; i < argc; ++i)
  {
    rip_vgm(argv[i]);
  }

  return 0;
}
