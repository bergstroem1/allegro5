/*         ______   ___    ___
 *        /\  _  \ /\_ \  /\_ \
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      PulseAudio sound driver.
 *
 *      By Matthew Leverton.
 *
 *      See readme.txt for copyright information.
 */

#include "allegro5/allegro.h"
#include "allegro5/internal/aintern_audio.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <malloc.h>

ALLEGRO_DEBUG_CHANNEL("PulseAudio")

typedef struct PULSEAUDIO_VOICE
{
  pa_simple *s;
  ALLEGRO_THREAD *poll_thread;

  volatile enum { PV_PLAYING, PV_STOPPING, PV_STOPPED } status;

  int frame_size; // number of bytes per frame 

  // direct buffer (non-streaming):
  ALLEGRO_MUTEX *buffer_mutex;  
  char *buffer, *buffer_end;
   
} PULSEAUDIO_VOICE;

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol,
   void *userdata)
{
   (void)c;
   (void)eol;
   pa_sink_state_t *ret = userdata;
   if (!i) return;
   *ret = i->state;
}

static int pulseaudio_open(void)
{
   /* Use PA_CONTEXT_NOAUTOSPAWN to see if a PA server is running.
    * If not, fail - we're better off using ALSA/OSS.
    * 
    * Also check for suspended PA - again better using ALSA/OSS in
    * that case (pa_simple_write just blocks until PA is unsuspended
    * otherwise).
    * 
    * TODO: Maybe we should have a force flag to the audio driver
    * open method, which in the case of PA would spawn a server if
    * none is running (and also unsuspend?).
    */

   pa_mainloop *mainloop = pa_mainloop_new();
   pa_context *c = pa_context_new(pa_mainloop_get_api(mainloop),
      al_get_app_name());
   if (!c) {
      pa_mainloop_free(mainloop);
      return 1;
   }

   pa_context_connect(c, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);

   while (1) {
      /* Don't block or it will hang if there is no server to connect to. */
      const bool blocking = 0;
      if (pa_mainloop_iterate(mainloop, blocking, NULL) < 0) {
         ALLEGRO_ERROR("pa_mainloop_iterate failed\n");
         pa_context_disconnect(c);
         pa_mainloop_free(mainloop);
         break;
      }
      pa_context_state_t s = pa_context_get_state(c);
      if (s == PA_CONTEXT_READY) {
         ALLEGRO_DEBUG("PA_CONTEXT_READY\n");
         break;
      }
      if (s == PA_CONTEXT_FAILED) {
         ALLEGRO_ERROR("PA_CONTEXT_FAILED\n");
         pa_context_disconnect(c);
         pa_mainloop_free(mainloop);
         return 1;
      }
   }

   pa_sink_state_t state = 0;
   pa_operation *op = pa_context_get_sink_info_list(c, sink_info_cb,
      &state);
   while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
      pa_mainloop_iterate(mainloop, 1, NULL);
   }
   /*if (state == PA_SINK_SUSPENDED) {
      pa_context_disconnect(c);
      pa_mainloop_free(mainloop);
      return 1;
   }*/
   pa_operation_unref(op);
   pa_context_disconnect(c);
   pa_context_unref(c);
   pa_mainloop_free(mainloop);
   return 0;
}

static void pulseaudio_close(void)
{
}

static void *pulseaudio_update(ALLEGRO_THREAD *self, void *data)
{
   ALLEGRO_VOICE *voice = data;
   PULSEAUDIO_VOICE *pv = voice->extra;

   while (!al_get_thread_should_stop(self)) {
      if (pv->status == PV_PLAYING) {
         unsigned int frames = 4096;
         if (voice->is_streaming) { 
            // streaming audio           
            const void *data = _al_voice_update(voice, &frames);
            if (data) {
               pa_simple_write(pv->s, data, frames * pv->frame_size, NULL);
            }
         }
         else {
            // direct buffer audio
            al_lock_mutex(pv->buffer_mutex);
            const char *data = pv->buffer;
            unsigned int len = frames * pv->frame_size;
            pv->buffer += frames * pv->frame_size;
            if (pv->buffer > pv->buffer_end) {
               len = pv->buffer_end - data;
               pv->buffer = voice->attached_stream->spl_data.buffer.ptr;
               voice->attached_stream->pos = 0;
               if (voice->attached_stream->loop == ALLEGRO_PLAYMODE_ONCE) {
                  pv->status = PV_STOPPING;
               }
            }
            else {
               voice->attached_stream->pos += frames;
            }
            al_unlock_mutex(pv->buffer_mutex);

            pa_simple_write(pv->s, data, len, NULL);
         }
      }
      else if (pv->status == PV_STOPPING) {
         pa_simple_flush(pv->s, NULL);
         pv->status = PV_STOPPED;
      }
      else if (pv->status == PV_STOPPED) {
         al_rest(0.001);
      }
   }

   return NULL;
}

static int pulseaudio_allocate_voice(ALLEGRO_VOICE *voice)
{
   PULSEAUDIO_VOICE *pv = al_malloc(sizeof(PULSEAUDIO_VOICE));
   pa_sample_spec ss;
   pa_buffer_attr ba;

   ss.channels = al_get_channel_count(voice->chan_conf);
   ss.rate = voice->frequency;

   if (voice->depth == ALLEGRO_AUDIO_DEPTH_UINT8)
      ss.format = PA_SAMPLE_U8;
   else if (voice->depth == ALLEGRO_AUDIO_DEPTH_INT16)
      ss.format = PA_SAMPLE_S16NE;
#if PA_API_VERSION > 11
   else if (voice->depth == ALLEGRO_AUDIO_DEPTH_INT24)
      ss.format = PA_SAMPLE_S24NE;
#endif
   else if (voice->depth == ALLEGRO_AUDIO_DEPTH_FLOAT32)
      ss.format = PA_SAMPLE_FLOAT32NE;
   else {
      ALLEGRO_ERROR("Unsupported PulseAudio sound format.\n");
      al_free(pv);
      return 1;
   }

   ba.maxlength = 0x10000; // maximum length of buffer
   ba.tlength   = 0x2000;  // target length of buffer
   ba.prebuf    = 0;       // minimum data size required before playback starts
   ba.minreq    = 0;       // minimum size of request 
   ba.fragsize  = -1;      // fragment size (recording)

   pv->s = pa_simple_new(NULL,         // Use the default server.
                   al_get_app_name(),     
                   PA_STREAM_PLAYBACK,
                   NULL,               // Use the default device.
                   "Allegro Voice",    
                   &ss,                
                   NULL,               // Use default channel map
                   &ba,                
                   NULL                // Ignore error code.
   );

   if (!pv->s) {
      al_free(pv);
      return 1;
   }

   voice->extra = pv;

   pv->frame_size = ss.channels * al_get_audio_depth_size(voice->depth);
   pv->status = PV_STOPPED;
   pv->buffer_mutex = al_create_mutex();

   pv->poll_thread = al_create_thread(pulseaudio_update, (void*)voice);
   al_start_thread(pv->poll_thread);

   return 0;
}

static void pulseaudio_deallocate_voice(ALLEGRO_VOICE *voice)
{
   PULSEAUDIO_VOICE *pv = voice->extra;

   /* We do NOT hold the voice mutex here, so this does NOT result in a
    * deadlock when the thread calls _al_voice_update.
    */
   al_set_thread_should_stop(pv->poll_thread);
   al_join_thread(pv->poll_thread, NULL);
   al_destroy_thread(pv->poll_thread);

   al_destroy_mutex(pv->buffer_mutex);

   pa_simple_free(pv->s);
   al_free(pv);
}

static int pulseaudio_load_voice(ALLEGRO_VOICE *voice, const void *data)
{
   PULSEAUDIO_VOICE *pv = voice->extra;

   if (voice->attached_stream->loop == ALLEGRO_PLAYMODE_BIDIR) {
      ALLEGRO_INFO("Backwards playing not supported by the driver.\n");
      return 1;
   }

   voice->attached_stream->pos = 0;

   pv->buffer = voice->attached_stream->spl_data.buffer.ptr;
   pv->buffer_end = pv->buffer + (voice->attached_stream->spl_data.len) * pv->frame_size;

   return 0;
   (void)data;
}

static void pulseaudio_unload_voice(ALLEGRO_VOICE *voice)
{
   (void) voice;
}

static int pulseaudio_start_voice(ALLEGRO_VOICE *voice)
{
   PULSEAUDIO_VOICE *pv = voice->extra;   
   pv->status = PV_PLAYING;
   
   return 0;
}

static int pulseaudio_stop_voice(ALLEGRO_VOICE *voice)
{
   PULSEAUDIO_VOICE *pv = voice->extra;
   pv->status = PV_STOPPING;

   while (pv->status == PV_STOPPING) {
      al_rest(0.001);
   }
   
   return 0;
}

static bool pulseaudio_voice_is_playing(const ALLEGRO_VOICE *voice)
{
   PULSEAUDIO_VOICE *pv = voice->extra;
   return pv->status == PV_PLAYING;
}

static unsigned int pulseaudio_get_voice_position(const ALLEGRO_VOICE *voice)
{
   return voice->attached_stream->pos;
}

static int pulseaudio_set_voice_position(ALLEGRO_VOICE *voice, unsigned int pos)
{
   PULSEAUDIO_VOICE *pv = voice->extra;

   pa_simple_drain(pv->s, NULL);
   
   al_lock_mutex(pv->buffer_mutex);
   voice->attached_stream->pos = pos;
   pv->buffer = (char*)voice->attached_stream->spl_data.buffer.ptr + pos * pv->frame_size;
   al_unlock_mutex(pv->buffer_mutex);

   return 0;
}

/* Recording */

typedef struct PULSEAUDIO_RECORDER {
   pa_simple *s;
   pa_sample_spec ss;
   pa_buffer_attr ba;
} PULSEAUDIO_RECORDER;

static void *pulse_audio_update_recorder(ALLEGRO_THREAD *t, void *data)
{
   ALLEGRO_AUDIO_RECORDER *r = (ALLEGRO_AUDIO_RECORDER *) data;
   PULSEAUDIO_RECORDER *pa = (PULSEAUDIO_RECORDER *) r->extra;
   ALLEGRO_EVENT user_event;
   uint8_t *null_buffer;
   unsigned int fragment_i = 0;
   
   null_buffer = al_malloc(1024);
   if (!null_buffer) {
      ALLEGRO_ERROR("Unable to create buffer for draining PulseAudio.\n");
      return NULL;
   }
   
   while (!al_get_thread_should_stop(t))
   {
      al_lock_mutex(r->mutex);
      if (!r->is_recording) {
         /* Even if not recording, we still want to read from the PA server.
            Otherwise it will buffer everything and spit it all out whenever
            the recording resumes. */
         al_unlock_mutex(r->mutex);
         pa_simple_read(pa->s, null_buffer, 1024, NULL);
      }
      else {
         ALLEGRO_AUDIO_RECORDER_EVENT *e;
         al_unlock_mutex(r->mutex);
         if (pa_simple_read(pa->s, r->fragments[fragment_i], r->fragment_size, NULL) >= 0) {
            user_event.user.type = ALLEGRO_EVENT_AUDIO_RECORDER_FRAGMENT;
            e = al_get_audio_recorder_event(&user_event);
            e->buffer = r->fragments[fragment_i];
            e->samples = r->samples;           
            al_emit_user_event(&r->source, &user_event, NULL);
         
            if (++fragment_i == r->fragment_count) {
               fragment_i = 0;
            }
         }
      }
   }

   al_free(null_buffer);
   return NULL;
};

static int pulseaudio_allocate_recorder(ALLEGRO_AUDIO_RECORDER *r)
{
   PULSEAUDIO_RECORDER *pa;
   
   pa = al_calloc(1, sizeof(*pa));
   if (!pa) {
     ALLEGRO_ERROR("Unable to allocate memory for PULSEAUDIO_RECORDER.\n");
     return 1;
   }
   
   pa->ss.channels = al_get_channel_count(r->chan_conf);
   pa->ss.rate = r->frequency;

   if (r->depth == ALLEGRO_AUDIO_DEPTH_UINT8) 
      pa->ss.format = PA_SAMPLE_U8;
   else if (r->depth == ALLEGRO_AUDIO_DEPTH_INT16)
      pa->ss.format = PA_SAMPLE_S16NE;
#if PA_API_VERSION > 11
   else if (r->depth == ALLEGRO_AUDIO_DEPTH_INT24)
      pa->ss.format = PA_SAMPLE_S24NE;
#endif
   else if (r->depth == ALLEGRO_AUDIO_DEPTH_FLOAT32)
      pa->ss.format = PA_SAMPLE_FLOAT32NE;
   else {
      ALLEGRO_ERROR("Unsupported PulseAudio sound format (depth).\n");
      al_free(pa);
      return 1;
   }
   
   /* maximum length of the PulseAudio buffer. -1 => let the server decide. */
   pa->ba.maxlength = -1;
   
   /* fragment size (bytes) controls how much data is returned back per read. 
      The documentation recommends -1 for default behavior, but that sets a
      latency of around 2 seconds. Lower value decreases latency but increases
      overhead. 
      
      The following attempts to set it (the base latency) to 1/8 of a second. 
    */
   pa->ba.fragsize = (r->sample_size * r->frequency) / 8;
   
   pa->s = pa_simple_new(NULL, al_get_app_name(), PA_STREAM_RECORD, NULL, "Allegro Audio Recorder", &pa->ss, NULL, &pa->ba, NULL);
   if (!pa->s) {
      ALLEGRO_ERROR("pa_simple_new() failed.\n");
      al_free(pa);
      return 1;
   }
   
   r->thread = al_create_thread(pulse_audio_update_recorder, r);
   r->extra = pa;
   
   return 0;   
};

static void pulseaudio_deallocate_recorder(ALLEGRO_AUDIO_RECORDER *r)
{
   PULSEAUDIO_RECORDER *pa = (PULSEAUDIO_RECORDER *) r->extra;
   
   pa_simple_free(pa->s);
   al_free(r->extra);
}

ALLEGRO_AUDIO_DRIVER _al_kcm_pulseaudio_driver =
{
   "PulseAudio",

   pulseaudio_open,
   pulseaudio_close,

   pulseaudio_allocate_voice,
   pulseaudio_deallocate_voice,

   pulseaudio_load_voice,
   pulseaudio_unload_voice,

   pulseaudio_start_voice,
   pulseaudio_stop_voice,

   pulseaudio_voice_is_playing,

   pulseaudio_get_voice_position,
   pulseaudio_set_voice_position,
   
   pulseaudio_allocate_recorder,
   pulseaudio_deallocate_recorder
};

/* vim: set sts=3 sw=3 et: */
