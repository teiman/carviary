/*
	snd_sdl.cpp -- SDL3 audio output (replaces snd_win.cpp)

	Uses a callback model: SDL calls us when it needs audio data.
	The engine mixes into a ring buffer; the callback copies from it.
*/

#include "quakedef.h"
#include <SDL3/SDL.h>

static SDL_AudioStream	*sdl_audiostream = NULL;
static SDL_AudioDeviceID sdl_audiodevice = 0;
static unsigned char	*sdl_dma_buffer = NULL;
static int				sdl_dma_bufsize = 0;
static volatile int		sdl_playpos = 0;		// playback position in mono samples

// Audio callback -- SDL calls this from its audio thread
static void SDLCALL sdl_audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
	if (!sdl_dma_buffer || additional_amount <= 0)
		return;

	int bytes_per_sample = sn.samplebits / 8;
	int total_bytes = sn.samples * bytes_per_sample;
	int pos = sdl_playpos * bytes_per_sample;
	int remaining = additional_amount;

	while (remaining > 0)
	{
		int avail = total_bytes - pos;
		int chunk = (remaining < avail) ? remaining : avail;

		SDL_PutAudioStreamData(stream, sdl_dma_buffer + pos, chunk);

		pos += chunk;
		if (pos >= total_bytes)
			pos = 0;
		remaining -= chunk;
	}

	sdl_playpos = pos / bytes_per_sample;
}

qboolean SNDDMA_Init (void)
{
	SDL_AudioSpec spec;
	int tmp;

	if (!SDL_WasInit(SDL_INIT_AUDIO))
	{
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
		{
			Con_Printf("SNDDMA_Init: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
			return false;
		}
	}

	// Desired format
	SDL_zero(spec);
	spec.freq     = 22050;
	spec.format   = SDL_AUDIO_S16;
	spec.channels = 2;

	tmp = COM_CheckParm("-sndspeed");
	if (tmp && tmp < com_argc - 1)
	{
		spec.freq = Q_atoi(com_argv[tmp + 1]);
		if (spec.freq <= 0)
			spec.freq = 22050;
	}

	// Open device
	sdl_audiodevice = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
	if (sdl_audiodevice == 0)
	{
		Con_Printf("SNDDMA_Init: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
		return false;
	}

	// Create stream with callback
	sdl_audiostream = SDL_CreateAudioStream(&spec, NULL);
	if (!sdl_audiostream)
	{
		Con_Printf("SNDDMA_Init: SDL_CreateAudioStream failed: %s\n", SDL_GetError());
		SDL_CloseAudioDevice(sdl_audiodevice);
		sdl_audiodevice = 0;
		return false;
	}

	SDL_SetAudioStreamGetCallback(sdl_audiostream, sdl_audio_callback, NULL);

	if (!SDL_BindAudioStream(sdl_audiodevice, sdl_audiostream))
	{
		Con_Printf("SNDDMA_Init: SDL_BindAudioStream failed: %s\n", SDL_GetError());
		SDL_DestroyAudioStream(sdl_audiostream);
		sdl_audiostream = NULL;
		SDL_CloseAudioDevice(sdl_audiodevice);
		sdl_audiodevice = 0;
		return false;
	}

	// Ring buffer: 16K frames * 2ch * 2bytes = 65536 bytes
	int frames     = 16384;
	int channels   = spec.channels;
	int samplebits = 16;
	int samples    = frames * channels;

	sdl_dma_bufsize = samples * (samplebits / 8);
	sdl_dma_buffer  = (unsigned char *)calloc(1, sdl_dma_bufsize);
	if (!sdl_dma_buffer)
	{
		Con_Printf("SNDDMA_Init: buffer alloc failed\n");
		SDL_DestroyAudioStream(sdl_audiostream);
		sdl_audiostream = NULL;
		SDL_CloseAudioDevice(sdl_audiodevice);
		sdl_audiodevice = 0;
		return false;
	}

	sn.splitbuffer      = 0;
	sn.samplebits       = samplebits;
	sn.speed            = spec.freq;
	sn.channels         = channels;
	sn.samples          = samples;
	sn.samplepos        = 0;
	sn.soundalive       = true;
	sn.gamealive        = true;
	sn.submission_chunk = 1;
	sn.buffer           = sdl_dma_buffer;

	shm = &sn;
	sdl_playpos = 0;

	Con_Printf("SDL3 audio initialised: %d Hz, %d-bit, %d ch\n",
		sn.speed, sn.samplebits, sn.channels);

	return true;
}

int SNDDMA_GetDMAPos (void)
{
	sn.samplepos = sdl_playpos;
	return sn.samplepos;
}

void SNDDMA_Submit (void)
{
	// Callback-driven: nothing to do here.
	// The callback pulls data from the ring buffer as SDL needs it.
}

void SNDDMA_Shutdown (void)
{
	if (sdl_audiostream)
	{
		SDL_DestroyAudioStream(sdl_audiostream);
		sdl_audiostream = NULL;
	}
	if (sdl_audiodevice)
	{
		SDL_CloseAudioDevice(sdl_audiodevice);
		sdl_audiodevice = 0;
	}
	if (sdl_dma_buffer)
	{
		free(sdl_dma_buffer);
		sdl_dma_buffer = NULL;
	}
	sdl_dma_bufsize = 0;
	sdl_playpos = 0;
}
