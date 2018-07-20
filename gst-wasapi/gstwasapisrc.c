/*
 * Copyright (C) 2008 Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>
 * Copyright (C) 2018 Centricular Ltd.
 *   Author: Nirbheek Chauhan <nirbheek@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-wasapisrc
 * @title: wasapisrc
 *
 * Provides audio capture from the Windows Audio Session API available with
 * Vista and newer.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 -v wasapisrc ! fakesink
 * ]| Capture from the default audio device and render to fakesink.
 *
 * |[
 * gst-launch-1.0 -v wasapisrc low-latency=true ! fakesink
 * ]| Capture from the default audio device with the minimum possible latency and render to fakesink.
 *
 */
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstwasapisrc.h"
#include <avrt.h>

#if defined(_MSC_VER)
#include <functiondiscoverykeys_devpkey.h>
#elif !defined(PKEY_Device_FriendlyName)
#include <initguid.h>
#include <propkey.h>
DEFINE_PROPERTYKEY (PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80,
    0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);
DEFINE_PROPERTYKEY (PKEY_AudioEngine_DeviceFormat, 0xf19f064d, 0x82c, 0x4e27,
    0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c, 0);
#endif

GST_DEBUG_CATEGORY_STATIC (gst_wasapi_src_debug);
#define GST_CAT_DEFAULT gst_wasapi_src_debug

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_WASAPI_STATIC_CAPS));

#define DEFAULT_ROLE          GST_WASAPI_DEVICE_ROLE_CONSOLE
#define DEFAULT_LOOPBACK      FALSE
#define DEFAULT_EXCLUSIVE     FALSE
#define DEFAULT_LOW_LATENCY   FALSE
#define DEFAULT_AUDIOCLIENT3  FALSE
/* The clock provided by WASAPI is always off and causes buffers to be late
 * very quickly on the sink. Disable pending further investigation. */
#define DEFAULT_PROVIDE_CLOCK FALSE

enum
{
  PROP_0,
  PROP_ROLE,
  PROP_DEVICE,
  PROP_LOOPBACK,
  PROP_EXCLUSIVE,
  PROP_LOW_LATENCY,
  PROP_AUDIOCLIENT3,
  PROP_RESTART_REQUIRED,
  PROP_SAMPLE_RATE,
  PROP_DEVICE_DESCRIPTION,
};

static void gst_wasapi_src_dispose (GObject * object);
static void gst_wasapi_src_finalize (GObject * object);
static void gst_wasapi_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_wasapi_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstCaps *gst_wasapi_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter);

static gboolean gst_wasapi_src_open (GstAudioSrc * asrc);
static gboolean gst_wasapi_src_close (GstAudioSrc * asrc);
static gboolean gst_wasapi_src_prepare (GstAudioSrc * asrc,
    GstAudioRingBufferSpec * spec);
static gboolean gst_wasapi_src_unprepare (GstAudioSrc * asrc);
static guint gst_wasapi_src_read (GstAudioSrc * asrc, gpointer data,
    guint length, GstClockTime * timestamp);
static guint gst_wasapi_src_delay (GstAudioSrc * asrc);
static void gst_wasapi_src_reset (GstAudioSrc * asrc);

#ifdef DEFAULT_PROVIDE_CLOCK
static GstClockTime gst_wasapi_src_get_time (GstClock * clock,
    gpointer user_data);
#endif

#define gst_wasapi_src_parent_class parent_class
G_DEFINE_TYPE (GstWasapiSrc, gst_wasapi_src, GST_TYPE_AUDIO_SRC);

static void
gst_wasapi_src_class_init (GstWasapiSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstAudioSrcClass *gstaudiosrc_class = GST_AUDIO_SRC_CLASS (klass);

  gobject_class->dispose = gst_wasapi_src_dispose;
  gobject_class->finalize = gst_wasapi_src_finalize;
  gobject_class->set_property = gst_wasapi_src_set_property;
  gobject_class->get_property = gst_wasapi_src_get_property;

  g_object_class_install_property (gobject_class,
      PROP_ROLE,
      g_param_spec_enum ("role", "Role",
          "Role of the device: communications, multimedia, etc",
          GST_WASAPI_DEVICE_TYPE_ROLE, DEFAULT_ROLE, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_DEVICE,
      g_param_spec_string ("device", "Device",
          "WASAPI playback device as a GUID string",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_LOOPBACK,
      g_param_spec_boolean ("loopback", "Loopback recording",
          "Open the sink device for loopback recording",
          DEFAULT_LOOPBACK, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_EXCLUSIVE,
      g_param_spec_boolean ("exclusive", "Exclusive mode",
          "Open the device in exclusive mode",
          DEFAULT_EXCLUSIVE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_LOW_LATENCY,
      g_param_spec_boolean ("low-latency", "Low latency",
          "Optimize all settings for lowest latency. Always safe to enable.",
          DEFAULT_LOW_LATENCY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_AUDIOCLIENT3,
      g_param_spec_boolean ("use-audioclient3", "Use the AudioClient3 API",
          "Whether to use the Windows 10 AudioClient3 API when available",
          DEFAULT_AUDIOCLIENT3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class,
    PROP_RESTART_REQUIRED,
    g_param_spec_boolean("restart-required", "Should we restart plugin",
      "EOS signals don't work so we need to hack around this",
      FALSE, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
      PROP_SAMPLE_RATE,
      g_param_spec_int("sample-rate", "Sample Rate",
          "Sample Rate in Hz",
          0, 1000000, 0, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
      PROP_DEVICE_DESCRIPTION,
      g_param_spec_string ("description", "Device Description",
          "Friendly Name of device ",
          NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (gstelement_class, &src_template);
  gst_element_class_set_static_metadata (gstelement_class, "WasapiSrc",
      "Source/Audio",
      "Stream audio from an audio capture device through WASAPI",
      "Nirbheek Chauhan <nirbheek@centricular.com>, "
      "Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>");

  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_wasapi_src_get_caps);

  gstaudiosrc_class->open = GST_DEBUG_FUNCPTR (gst_wasapi_src_open);
  gstaudiosrc_class->close = GST_DEBUG_FUNCPTR (gst_wasapi_src_close);
  gstaudiosrc_class->read = GST_DEBUG_FUNCPTR (gst_wasapi_src_read);
  gstaudiosrc_class->prepare = GST_DEBUG_FUNCPTR (gst_wasapi_src_prepare);
  gstaudiosrc_class->unprepare = GST_DEBUG_FUNCPTR (gst_wasapi_src_unprepare);
  gstaudiosrc_class->delay = GST_DEBUG_FUNCPTR (gst_wasapi_src_delay);
  gstaudiosrc_class->reset = GST_DEBUG_FUNCPTR (gst_wasapi_src_reset);

  GST_DEBUG_CATEGORY_INIT (gst_wasapi_src_debug, "wasapisrc",
      0, "Windows audio session API source");
}

static void
gst_wasapi_src_init (GstWasapiSrc * self)
{

#ifdef DEFAULT_PROVIDE_CLOCK
  /* override with a custom clock */
  if (GST_AUDIO_BASE_SRC (self)->clock)
    gst_object_unref (GST_AUDIO_BASE_SRC (self)->clock);

  GST_AUDIO_BASE_SRC (self)->clock = gst_audio_clock_new ("GstWasapiSrcClock",
      gst_wasapi_src_get_time, gst_object_ref (self),
      (GDestroyNotify) gst_object_unref);
#endif

  self->sample_rate = 0;
  self->device_description = NULL;
  self->role = DEFAULT_ROLE;
  self->sharemode = AUDCLNT_SHAREMODE_SHARED;
  self->loopback = DEFAULT_LOOPBACK;
  self->low_latency = DEFAULT_LOW_LATENCY;
  self->try_audioclient3 = DEFAULT_AUDIOCLIENT3;
  self->event_handle = CreateEvent (NULL, FALSE, FALSE, NULL);
  self->stop_handle = CreateEvent (NULL, FALSE, FALSE, NULL);
  self->client_needs_restart = FALSE;
  self->capture_too_many_frames_log_count = 0;
  self->client_needs_restart = FALSE;
  self->change_initialized = 0;
  self->eos_sent = FALSE;
  CoInitialize (NULL);
}

static void
gst_wasapi_src_dispose (GObject * object)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  if (self->event_handle != NULL) {
    CloseHandle (self->event_handle);
    self->event_handle = NULL;
  }

  if (self->stop_handle != NULL) {
    CloseHandle (self->stop_handle);
    self->stop_handle = NULL;
  }

  if (self->client_clock != NULL) {
    IUnknown_Release (self->client_clock);
    self->client_clock = NULL;
  }

  if (self->client != NULL) {
    IUnknown_Release (self->client);
    self->client = NULL;
  }

  if (self->capture_client != NULL) {
    IUnknown_Release (self->capture_client);
    self->capture_client = NULL;
  }

  if (self->change_initialized && self->change.client.lpVtbl) {
    change_notify * change = &self->change;
    self->change_initialized = 0;
    IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(
      change->pEnumerator, (IMMNotificationClient *)change);
    IUnknown_Release(change->pEnumerator);
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_wasapi_src_finalize (GObject * object)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  CoTaskMemFree (self->mix_format);
  self->mix_format = NULL;

  CoUninitialize ();

  g_clear_pointer (&self->cached_caps, gst_caps_unref);
  g_clear_pointer (&self->positions, g_free);
  g_clear_pointer (&self->device_strid, g_free);
  g_clear_pointer (&self->device_description, g_free);
  self->sample_rate = 0;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_wasapi_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  switch (prop_id) {
    case PROP_ROLE:
      self->role = gst_wasapi_device_role_to_erole (g_value_get_enum (value));
      break;
    case PROP_DEVICE:
    {
      const gchar *device = g_value_get_string (value);
      g_free (self->device_strid);
      self->device_strid =
          device ? g_utf8_to_utf16 (device, -1, NULL, NULL, NULL) : NULL;
      break;
    }
    case PROP_LOOPBACK:
      self->loopback = g_value_get_boolean (value);
      break;
    case PROP_EXCLUSIVE:
      self->sharemode = g_value_get_boolean (value)
          ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
      break;
    case PROP_LOW_LATENCY:
      self->low_latency = g_value_get_boolean (value);
      break;
    case PROP_AUDIOCLIENT3:
      self->try_audioclient3 = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wasapi_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (object);

  switch (prop_id) {
    case PROP_ROLE:
      g_value_set_enum (value, gst_wasapi_erole_to_device_role (self->role));
      break;
    case PROP_DEVICE:
      g_value_take_string (value, self->device_strid ?
          g_utf16_to_utf8 (self->device_strid, -1, NULL, NULL, NULL) : NULL);
      break;
    case PROP_LOOPBACK:
      g_value_set_boolean (value, self->loopback);
      break;
    case PROP_EXCLUSIVE:
      g_value_set_boolean (value,
          self->sharemode == AUDCLNT_SHAREMODE_EXCLUSIVE);
      break;
    case PROP_LOW_LATENCY:
      g_value_set_boolean (value, self->low_latency);
      break;
    case PROP_AUDIOCLIENT3:
      g_value_set_boolean (value, self->try_audioclient3);
      break;
    case PROP_RESTART_REQUIRED:
      g_value_set_boolean(value, self->eos_sent);
      break;
    case PROP_SAMPLE_RATE:
      g_value_set_int(value, self->sample_rate);
      break;
    case PROP_DEVICE_DESCRIPTION:
      g_value_set_string (value, self->device_description);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_wasapi_src_can_audioclient3 (GstWasapiSrc * self)
{
  if (self->sharemode == AUDCLNT_SHAREMODE_SHARED &&
      self->try_audioclient3 && gst_wasapi_util_have_audioclient3 ())
    return TRUE;
  return FALSE;
}

static GstCaps *
gst_wasapi_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (bsrc);
  WAVEFORMATEX *format = NULL;
  GstCaps *caps = NULL;

  GST_DEBUG_OBJECT (self, "entering get caps");

  if (self->cached_caps) {
    caps = gst_caps_ref (self->cached_caps);
  } else {
    GstCaps *template_caps;
    gboolean ret;

    template_caps = gst_pad_get_pad_template_caps (bsrc->srcpad);

    if (!self->client) {
      caps = template_caps;
      goto out;
    }

    ret = gst_wasapi_util_get_device_format (GST_ELEMENT (self),
        self->sharemode, self->device, self->client, &format);
    if (!ret) {
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
          ("failed to detect format"));
      gst_caps_unref (template_caps);
      return NULL;
    }

    gst_wasapi_util_parse_waveformatex ((WAVEFORMATEXTENSIBLE *) format,
        template_caps, &caps, &self->positions);
    if (caps == NULL) {
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unknown format"));
      gst_caps_unref (template_caps);
      return NULL;
    }

    {
      gchar *pos_str = gst_audio_channel_positions_to_string (self->positions,
          format->nChannels);
      GST_INFO_OBJECT (self, "positions are: %s", pos_str);
      g_free (pos_str);
    }

    self->mix_format = format;
    gst_caps_replace (&self->cached_caps, caps);
    gst_caps_unref (template_caps);
  }

  if (filter) {
    GstCaps *filtered =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = filtered;
  }

out:
  GST_DEBUG_OBJECT (self, "returning caps %" GST_PTR_FORMAT, caps);
  return caps;
}


static gboolean
gst_wasapi_src_open (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  gboolean res = FALSE;
  IAudioClient *client = NULL;
  IMMDevice *device = NULL;
  IPropertyStore *prop_store = NULL;

  if (self->client)
    return TRUE;

  /* FIXME: Switching the default device does not switch the stream to it,
   * even if the old device was unplugged. We need to handle this somehow.
   * For example, perhaps we should automatically switch to the new device if
   * the default device is changed and a device isn't explicitly selected. */
  if (!gst_wasapi_util_get_device_client (GST_ELEMENT (self),
          self->loopback ? eRender : eCapture, self->role, self->device_strid,
          &device, &client)) {
    if (!self->device_strid)
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ, (NULL),
          ("Failed to get default device"));
    else
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ, (NULL),
          ("Failed to open device %S", self->device_strid));
    goto beach;
  }
  if (!self->device_strid) {
    gst_wasapi_util_initialize_notification_client(self);
  }
  self->client = client;
  self->device = device;
  res = TRUE;

  HRESULT hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &prop_store);
  if (hr != S_OK) {
      goto beach;
  }
  
  PROPVARIANT var;
  PropVariantInit (&var);
  hr = IPropertyStore_GetValue (prop_store, &PKEY_Device_FriendlyName, &var);
  if (hr != S_OK) {
    goto beach;
  }
  self->device_description = g_utf16_to_utf8 (var.pwszVal, -1, NULL, NULL, NULL);
  PropVariantClear (&var);
  GST_INFO_OBJECT (self, "device description %s", self->device_description);
    

beach:

  if (prop_store) {
    IUnknown_Release (prop_store);
  }
  return res;
}

static gboolean
gst_wasapi_src_close (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);

  if (self->device != NULL) {
    IUnknown_Release (self->device);
    self->device = NULL;
  }

  if (self->client != NULL) {
    IUnknown_Release (self->client);
    self->client = NULL;
  }

  return TRUE;
}

static gboolean
gst_wasapi_src_prepare (GstAudioSrc * asrc, GstAudioRingBufferSpec * spec)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  gboolean res = FALSE;
  REFERENCE_TIME latency_rt;
  guint bpf, rate, devicep_frames, buffer_frames;
  HRESULT hr;

  CoInitialize (NULL);

  if (gst_wasapi_src_can_audioclient3 (self)) {
    if (!gst_wasapi_util_initialize_audioclient3 (GST_ELEMENT (self), spec,
            (IAudioClient3 *) self->client, self->mix_format, self->low_latency,
            self->loopback, &devicep_frames))
      goto beach;
  } else {
    if (!gst_wasapi_util_initialize_audioclient (GST_ELEMENT (self), spec,
            self->client, self->mix_format, self->sharemode, self->low_latency,
            self->loopback, &devicep_frames))
      goto beach;
  }

  bpf = GST_AUDIO_INFO_BPF (&spec->info);
  rate = GST_AUDIO_INFO_RATE (&spec->info);

  /* Total size in frames of the allocated buffer that we will read from */
  hr = IAudioClient_GetBufferSize (self->client, &buffer_frames);
  HR_FAILED_GOTO (hr, IAudioClient::GetBufferSize, beach);

  GST_INFO_OBJECT (self, "buffer size is %i frames, device period is %i "
      "frames, bpf is %i bytes, rate is %i Hz", buffer_frames,
      devicep_frames, bpf, rate);
  self->sample_rate = rate;

  /* Actual latency-time/buffer-time will be different now */
  spec->segsize = devicep_frames * bpf;

  /* We need a minimum of 2 segments to ensure glitch-free playback */
  spec->segtotal = MAX (self->buffer_frame_count * bpf / spec->segsize, 2);

  GST_INFO_OBJECT (self, "segsize is %i, segtotal is %i", spec->segsize,
      spec->segtotal);

  self->overflow_buffer_size = spec->segsize * 4;
  self->overflow_buffer_ptr = 0;
  self->overflow_buffer_length = 0;
  self->overflow_buffer = g_malloc(self->overflow_buffer_size);

  /* Get WASAPI latency for logging */
  hr = IAudioClient_GetStreamLatency (self->client, &latency_rt);
  HR_FAILED_GOTO (hr, IAudioClient::GetStreamLatency, beach);

  GST_INFO_OBJECT (self, "wasapi stream latency: %" G_GINT64_FORMAT " (%"
      G_GINT64_FORMAT " ms)", latency_rt, latency_rt / 10000);

  /* Set the event handler which will trigger reads */
  hr = IAudioClient_SetEventHandle (self->client, self->event_handle);
  HR_FAILED_GOTO (hr, IAudioClient::SetEventHandle, beach);

  /* Get the clock and the clock freq */
  if (!gst_wasapi_util_get_clock (GST_ELEMENT (self), self->client,
          &self->client_clock))
    goto beach;

  hr = IAudioClock_GetFrequency (self->client_clock, &self->client_clock_freq);
  HR_FAILED_GOTO (hr, IAudioClock::GetFrequency, beach);

  GST_INFO_OBJECT (self, "wasapi clock freq is %" G_GUINT64_FORMAT,
      self->client_clock_freq);

  /* Get capture source client and start it up */
  if (!gst_wasapi_util_get_capture_client (GST_ELEMENT (self), self->client,
          &self->capture_client)) {
    goto beach;
  }

  hr = IAudioClient_Start (self->client);
  HR_FAILED_GOTO (hr, IAudioClock::Start, beach);

  gst_audio_ring_buffer_set_channel_positions (GST_AUDIO_BASE_SRC
      (self)->ringbuffer, self->positions);

  /* Increase the thread priority to reduce glitches */
  self->thread_priority_handle = gst_wasapi_util_set_thread_characteristics ();

  res = TRUE;
beach:
  /* unprepare() is not called if prepare() fails, but we want it to be, so call
   * it manually when needed */
  if (!res)
    gst_wasapi_src_unprepare (asrc);

  return res;
}

static gboolean
gst_wasapi_src_unprepare (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);

  if (self->thread_priority_handle != NULL) {
    gst_wasapi_util_revert_thread_characteristics
        (self->thread_priority_handle);
    self->thread_priority_handle = NULL;
  }

  if (self->client != NULL) {
    IAudioClient_Stop (self->client);
  }

  if (self->capture_client != NULL) {
    IUnknown_Release (self->capture_client);
    self->capture_client = NULL;
  }

  if (self->client_clock != NULL) {
    IUnknown_Release (self->client_clock);
    self->client_clock = NULL;
  }

  self->client_clock_freq = 0;
  self->capture_too_many_frames_log_count = 0;

  if (self->overflow_buffer != NULL) {
    g_free(self->overflow_buffer);
    self->overflow_buffer = NULL;
    self->overflow_buffer_size = 0;
    self->overflow_buffer_ptr = 0;
    self->overflow_buffer_length = 0;
  }

  CoUninitialize ();

  return TRUE;
}

static guint
gst_wasapi_src_read (GstAudioSrc * asrc, gpointer data, guint length,
    GstClockTime * timestamp)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  HRESULT hr;
  gint16 *from = NULL;
  guint wanted = length;
  DWORD flags;
  guint8 *data_ptr = data;

  GST_OBJECT_LOCK (self);
  if (self->client_needs_restart) {
    hr = IAudioClient_Start (self->client);
    HR_FAILED_AND (hr, IAudioClient::Start, length = 0; goto beach);
    self->client_needs_restart = FALSE;
  }
  GST_OBJECT_UNLOCK (self);

  if (G_UNLIKELY(self->overflow_buffer_length > 0)) {
      guint n = MIN(self->overflow_buffer_length, wanted);

      gpointer ptr = &(self->overflow_buffer[self->overflow_buffer_ptr]);
      memcpy(data_ptr, ptr, n);
      data_ptr += n;
      wanted -= n;

      GST_LOG_OBJECT(self, "restored %i bytes from overflow", n);
      if (n == self->overflow_buffer_length) {
          self->overflow_buffer_ptr = 0;
          self->overflow_buffer_length = 0;
      } else {
          GST_WARNING_OBJECT(self, "WASAPI more in overflow that wanted");
          self->overflow_buffer_ptr += n;
          self->overflow_buffer_length -= n;
      }
  }

  while (wanted > 0) {
    DWORD dwWaitResult;
    guint have_frames, n_frames, want_frames, read_len;

    /* Wait for data to become available */

    HANDLE events[2] = {
      self->event_handle,
      self->stop_handle
    };
    dwWaitResult = WaitForMultipleObjects (2, events, FALSE, INFINITE);
    if (!self->device_strid && g_atomic_int_get(&(self->change.default_changed))) {
      goto device_disappeared;
    }
    switch (dwWaitResult) {
      case WAIT_OBJECT_0:
        break;
      case WAIT_OBJECT_0 + 1: // Received a stop signal, going to fill data with silent
        memset (data_ptr, 0, wanted);
        goto beach;
      default:
        GST_ERROR_OBJECT (self, "Error waiting for event handle: %x",
            (guint) dwWaitResult);
        length = 0;
        goto beach;
    }

    // fully drain the wasapi driver - we may not get a new signal for pending buffers
    // https://blogs.msdn.microsoft.com/matthew_van_eerde/2014/11/05/draining-the-wasapi-capture-buffer-fully/
    int i = 0;
    while (TRUE) {

        hr = IAudioCaptureClient_GetBuffer(self->capture_client,
            (BYTE **)& from, &have_frames, &flags, NULL, NULL);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            goto device_disappeared;
        }
        else if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            gchar *msg = gst_wasapi_util_hresult_to_string(hr);
            GST_LOG_OBJECT(self, "IAudioCaptureClient::GetBuffer failed: %s"
                ", retrying later", msg);
            break;
        }
        else if (hr != S_OK) {
            gchar *msg = gst_wasapi_util_hresult_to_string(hr);
            GST_ERROR_OBJECT(self, "IAudioCaptureClient::GetBuffer failed: %s", msg);
            g_free(msg);
            length = 0;
            goto beach;
        }
        if (i > 0) {
            GST_INFO_OBJECT(self, "draining WASAPI buffer %i", i);
        }
        i++;

        int mask_handled = MAXINT ^ (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY | AUDCLNT_BUFFERFLAGS_SILENT);
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            GST_WARNING_OBJECT(self, "WASAPI reported glitch in buffer");
        } 
        
        if ((flags & mask_handled) != 0) {
            GST_INFO_OBJECT(self, "buffer flags=%#08x", (guint)flags);
        }

        want_frames = wanted / self->mix_format->nBlockAlign;

        /* Only copy data that will fit into the allocated buffer of size @length */
        n_frames = MIN (have_frames, want_frames);
        read_len = n_frames * self->mix_format->nBlockAlign;

        {
          guint bpf = self->mix_format->nBlockAlign;
          GST_LOG_OBJECT (self, "have: %i (%i bytes), can read: %i (%i bytes), "
              "will read: %i (%i bytes)", have_frames, have_frames * bpf,
              want_frames, wanted, n_frames, read_len);
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            memset(data_ptr, 0, read_len);
        } else {
            memcpy(data_ptr, from, read_len);
        }

        data_ptr += read_len; // we loop - so we need to also advance data
        wanted -= read_len;

        /* save to overflow if we got more data from the driver than we have room for */
        if (G_UNLIKELY(have_frames > want_frames)) {
            guint save_frames = have_frames - want_frames;
            gsize save_length = save_frames * self->mix_format->nBlockAlign;

            if ((self->overflow_buffer_length + save_length + self->overflow_buffer_ptr) >
                self->overflow_buffer_size) {
                GST_ERROR_OBJECT(self, "can't save overflow at: %i length: %i bytes want %i more bytes space is %i",
                                 self->overflow_buffer_ptr, self->overflow_buffer_length, save_length, self->overflow_buffer_size);
            } else {
                gpointer write_ptr = &(self->overflow_buffer[self->overflow_buffer_length]);
                guint8 * from_buffer = (guint8 *) from;
                gpointer from_ptr = &(from_buffer[read_len]);
                memcpy(write_ptr, from_ptr, save_length);
                self->overflow_buffer_length += (guint) save_length;
                GST_LOG_OBJECT(self, "saved %i bytes to overflow", save_length);
            }
        }

        /* Always release all captured buffers if we've captured any at all */
        hr = IAudioCaptureClient_ReleaseBuffer (self->capture_client, have_frames);
        HR_FAILED_AND (hr, IAudioClock::ReleaseBuffer, goto beach);
    }
  }


beach:
  // TODO: We need to properly buffer the results from GetBuffer because they return
  // an arbitrary amount of audio samples.  However, if we never empty this thing out
  // USB audio devices will glitch out after they fill up.  Right now, if there is 
  // extra samples coming from the device we will just drop them.
  // The guard is hr == S_OK because we don't want to try pulling frames if we came
  // here because there was an error.
#if 0
  while (hr == S_OK) {
    guint have_frames;
    hr = IAudioCaptureClient_GetBuffer(self->capture_client,
      (BYTE **)& from, &have_frames, &flags, NULL, NULL);
    if (hr == S_OK) {
      GST_WARNING("Buffer Not Empty. Dropping audio frames");
      IAudioCaptureClient_ReleaseBuffer(self->capture_client, have_frames);
    }
  }
#endif
  return length;
device_disappeared:
  {
    if (!self->eos_sent) {
      GST_INFO_OBJECT(asrc, "The audio device has been disconnected.");
      gboolean success = gst_element_post_message(GST_ELEMENT(self),
        gst_message_new_element(GST_OBJECT(self),
          gst_structure_new("wasapi_restart",
            NULL)));
      if (!success) {
        GST_WARNING("Unable to send message");
      }
      self->eos_sent = TRUE;
    }
    return (guint) length;
  }
}

static guint
gst_wasapi_src_delay (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  guint delay = 0;
  HRESULT hr;

  hr = IAudioClient_GetCurrentPadding (self->client, &delay);
  HR_FAILED_RET (hr, IAudioClock::GetCurrentPadding, 0);

  return delay;
}

static void
gst_wasapi_src_reset (GstAudioSrc * asrc)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (asrc);
  HRESULT hr;

  SetEvent (self->stop_handle);

  if (!self->client)
    return;

  GST_OBJECT_LOCK (self);
  hr = IAudioClient_Stop (self->client);
  HR_FAILED_RET (hr, IAudioClock::Stop,);

  hr = IAudioClient_Reset (self->client);
  HR_FAILED_RET (hr, IAudioClock::Reset,);

  self->client_needs_restart = TRUE;
  GST_OBJECT_UNLOCK (self);
}

#ifdef DEFAULT_PROVIDE_CLOCK
static GstClockTime
gst_wasapi_src_get_time (GstClock * clock, gpointer user_data)
{
  GstWasapiSrc *self = GST_WASAPI_SRC (user_data);
  HRESULT hr;
  guint64 devpos;
  GstClockTime result;

  if (G_UNLIKELY (self->client_clock == NULL))
    return GST_CLOCK_TIME_NONE;

  hr = IAudioClock_GetPosition (self->client_clock, &devpos, NULL);
  HR_FAILED_RET (hr, IAudioClock::GetPosition, GST_CLOCK_TIME_NONE);

  result = gst_util_uint64_scale_int (devpos, GST_SECOND,
      self->client_clock_freq);

  /*
     GST_DEBUG_OBJECT (self, "devpos = %" G_GUINT64_FORMAT
     " frequency = %" G_GUINT64_FORMAT
     " result = %" G_GUINT64_FORMAT " ms",
     devpos, self->client_clock_freq, GST_TIME_AS_MSECONDS (result));
   */

  return result;
}
#endif
