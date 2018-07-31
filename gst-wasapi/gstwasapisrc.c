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

#include <gst/gst.h>
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

#define GETTEXT_PACKAGE gst-plugins-bebo
#include "gst-i18n-plugin.h"

#define GST_AUDIO_BASE_SRC_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_AUDIO_BASE_SRC, GstAudioBaseSrcPrivate))

struct _GstAudioBaseSrcPrivate
{
  /* the clock slaving algorithm in use */
  GstAudioBaseSrcSlaveMethod slave_method;
};

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
#define DEFAULT_DRIFT_CORRECTION_THRESHOLD  50 * 100000 // 50ms

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
  PROP_TIMESHIFTED_COUNT,
  PROP_DRIFT_CORRECTION_COUNT,
  PROP_DRIFT_CORRECTION_THRESHOLD,
};

static GstFlowReturn gst_audio_base_src_create (GstBaseSrc * bsrc,
    guint64 offset, guint length, GstBuffer ** buf);

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

  gstbasesrc_class->create = GST_DEBUG_FUNCPTR (gst_audio_base_src_create);

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

  g_object_class_install_property (gobject_class,
      PROP_TIMESHIFTED_COUNT,
      g_param_spec_uint64("timeshifted-count", "Timeshifted buffer count",
          "Number of buffer got timeshifted",
          0, G_MAXUINT64, 0, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
      PROP_DRIFT_CORRECTION_COUNT,
      g_param_spec_uint64("drift-correction-count", "Drifted buffer count",
          "Number of buffer that is difted more than drift correction threshold",
          0, G_MAXUINT64, 0, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class,
      PROP_DRIFT_CORRECTION_THRESHOLD,
      g_param_spec_uint64("drift-correction-threshold", "Drifted buffer threshold (nanoseconds)",
          "The threshold in nanoseconds for when we start correcting drifted buffers",
          0, G_MAXUINT64, DEFAULT_DRIFT_CORRECTION_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

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
  self->initial_timestamp_diff = 0;
  self->timeshifted_count = 0;
  self->drift_correction_count = 0;
  self->drift_correction_threshold = DEFAULT_DRIFT_CORRECTION_THRESHOLD;
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
    case PROP_DRIFT_CORRECTION_THRESHOLD:
      self->drift_correction_threshold = g_value_get_uint64 (value);
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
    case PROP_TIMESHIFTED_COUNT:
      g_value_set_uint64 (value, self->timeshifted_count);
      break;
    case PROP_DRIFT_CORRECTION_COUNT:
      g_value_set_uint64 (value, self->drift_correction_count);
      break;
    case PROP_DRIFT_CORRECTION_THRESHOLD:
      g_value_set_uint64 (value, self->drift_correction_threshold);
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
  spec->segtotal = MAX (buffer_frames * bpf / spec->segsize, 2) + 1;

  GST_INFO_OBJECT (self, "segsize is %i, segtotal is %i (%i)", spec->segsize,
      spec->segtotal,
      buffer_frames * bpf / spec->segsize);

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

static guint64
gst_audio_base_src_get_offset (GstAudioBaseSrc * src)
{
  guint64 sample;
  gint readseg, segdone, segtotal, sps;
  gint diff;

  /* assume we can append to the previous sample */
  sample = src->next_sample;

  sps = src->ringbuffer->samples_per_seg;
  segtotal = src->ringbuffer->spec.segtotal;

  /* get the currently processed segment */
  segdone = g_atomic_int_get (&src->ringbuffer->segdone)
      - src->ringbuffer->segbase;

  if (sample != -1) {
    GST_DEBUG_OBJECT (src, "at segment %d and sample %" G_GUINT64_FORMAT,
        segdone, sample);
    /* figure out the segment and the offset inside the segment where
     * the sample should be read from. */
    readseg = sample / sps;

    /* See how far away it is from the read segment. Normally, segdone (where
     * new data is written in the ringbuffer) is bigger than readseg
     * (where we are reading). */
    diff = segdone - readseg;
    if (diff >= segtotal) {
      GST_DEBUG_OBJECT (src, "dropped, align to segment %d", segdone);
      /* sample would be dropped, position to next playable position */
      sample = ((guint64) (segdone)) * sps;
    }
  } else {
    /* no previous sample, go to the current position */
    GST_DEBUG_OBJECT (src, "first sample, align to current %d", segdone);
    sample = ((guint64) (segdone)) * sps;
    readseg = segdone;
  }

  GST_DEBUG_OBJECT (src,
      "reading from %d, we are at %d, sample %" G_GUINT64_FORMAT, readseg,
      segdone, sample);

  return sample;
}

static GstFlowReturn
gst_audio_base_src_create (GstBaseSrc * bsrc, guint64 offset, guint length,
    GstBuffer ** outbuf)
{
  GstAudioBaseSrc *src = GST_AUDIO_BASE_SRC (bsrc);
  GstWasapiSrc *self = GST_WASAPI_SRC (bsrc);
  GstFlowReturn ret;
  GstBuffer *buf;
  GstMapInfo info;
  guint8 *ptr;
  guint samples, total_samples;
  guint64 sample;
  gint bpf, rate;
  GstAudioRingBuffer *ringbuffer;
  GstAudioRingBufferSpec *spec;
  guint read;
  GstClockTime timestamp, duration;
  GstClockTime rb_timestamp = GST_CLOCK_TIME_NONE;
  GstClock *clock;
  gboolean first;
  gboolean first_sample = src->next_sample == -1;

  ringbuffer = src->ringbuffer;
  spec = &ringbuffer->spec;

  if (G_UNLIKELY (!gst_audio_ring_buffer_is_acquired (ringbuffer)))
    goto wrong_state;

  bpf = GST_AUDIO_INFO_BPF (&spec->info);
  rate = GST_AUDIO_INFO_RATE (&spec->info);

  if ((length == 0 && bsrc->blocksize == 0) || length == -1)
    /* no length given, use the default segment size */
    length = spec->segsize;
  else
    /* make sure we round down to an integral number of samples */
    length -= length % bpf;

  /* figure out the offset in the ringbuffer */
  if (G_UNLIKELY (offset != -1)) {
    sample = offset / bpf;
    /* if a specific offset was given it must be the next sequential
     * offset we expect or we fail for now. */
    if (src->next_sample != -1 && sample != src->next_sample)
      goto wrong_offset;
  } else {
    /* Calculate the sequentially-next sample we need to read. This can jump and
     * create a DISCONT. */
    sample = gst_audio_base_src_get_offset (src);
  }

  GST_DEBUG_OBJECT (src, "reading from sample %" G_GUINT64_FORMAT " length %u",
      sample, length);

  /* get the number of samples to read */
  total_samples = samples = length / bpf;

  /* use the basesrc allocation code to use bufferpools or custom allocators */
  ret = GST_BASE_SRC_CLASS (parent_class)->alloc (bsrc, offset, length, &buf);
  if (G_UNLIKELY (ret != GST_FLOW_OK))
    goto alloc_failed;

  gst_buffer_map (buf, &info, GST_MAP_WRITE);
  ptr = info.data;
  first = TRUE;
  do {
    GstClockTime tmp_ts = GST_CLOCK_TIME_NONE;

    read =
        gst_audio_ring_buffer_read (ringbuffer, sample, ptr, samples, &tmp_ts);
    if (first && GST_CLOCK_TIME_IS_VALID (tmp_ts)) {
      first = FALSE;
      rb_timestamp = tmp_ts;
    }
    GST_DEBUG_OBJECT (src, "read %u of %u", read, samples);
    /* if we read all, we're done */
    if (read == samples)
      break;

    if (g_atomic_int_get (&ringbuffer->state) ==
        GST_AUDIO_RING_BUFFER_STATE_ERROR)
      goto got_error;

    /* else something interrupted us and we wait for playing again. */
    GST_DEBUG_OBJECT (src, "wait playing");
    if (gst_base_src_wait_playing (bsrc) != GST_FLOW_OK)
      goto stopped;

    GST_DEBUG_OBJECT (src, "continue playing");

    /* read next samples */
    sample += read;
    samples -= read;
    ptr += read * bpf;
  } while (TRUE);
  gst_buffer_unmap (buf, &info);

  /* mark discontinuity if needed */
  if (G_UNLIKELY (sample != src->next_sample) && src->next_sample != -1) {
    GST_WARNING_OBJECT (src,
        "create DISCONT of %" G_GUINT64_FORMAT " samples at sample %"
        G_GUINT64_FORMAT, sample - src->next_sample, sample);
    GST_ELEMENT_WARNING (src, CORE, CLOCK,
        (_("Can't record audio fast enough")),
        ("Dropped %" G_GUINT64_FORMAT " samples. This is most likely because "
            "downstream can't keep up and is consuming samples too slowly.",
            sample - src->next_sample));
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
  }

  src->next_sample = sample + samples;

  /* get the normal timestamp to get the duration. */
  timestamp = gst_util_uint64_scale_int (sample, GST_SECOND, rate);
  duration = gst_util_uint64_scale_int (src->next_sample, GST_SECOND,
      rate) - timestamp;

  GST_OBJECT_LOCK (src);
  if (!(clock = GST_ELEMENT_CLOCK (src)))
    goto no_sync;

  if (!GST_CLOCK_TIME_IS_VALID (rb_timestamp) && clock != src->clock) {
    /* we are slaved, check how to handle this */
    switch (src->priv->slave_method) {
      case GST_AUDIO_BASE_SRC_SLAVE_RESAMPLE:
        /* Not implemented, use skew algorithm. This algorithm should
         * work on the readout pointer and produce more or less samples based
         * on the clock drift */
      {
        GstClockTime running_time;
        GstClockTime base_time;
        GstClockTime current_time;
        guint64 running_time_sample;
        gint running_time_segment;
        gint last_read_segment;
        gint segment_skew;
        gint sps;
        gint segments_written;
        gint last_written_segment;
        gboolean drift_correction = FALSE;

        /* get the amount of segments written from the device by now */
        segments_written = g_atomic_int_get (&ringbuffer->segdone);

        /* subtract the base to segments_written to get the number of the
         * last written segment in the ringbuffer
         * (one segment written = segment 0) */
        last_written_segment = segments_written - ringbuffer->segbase - 1;

        /* samples per segment */
        sps = ringbuffer->samples_per_seg;

        /* get the current time */
        current_time = gst_clock_get_time (clock);

        /* get the basetime */
        base_time = GST_ELEMENT_CAST (src)->base_time;

        /* get the running_time */
        running_time = current_time - base_time;

        /* the running_time converted to a sample
         * (relative to the ringbuffer) */
        running_time_sample =
            gst_util_uint64_scale_int (running_time, rate, GST_SECOND);

        /* the segmentnr corresponding to running_time, round down */
        running_time_segment = running_time_sample / sps;

        /* the segment currently read from the ringbuffer */
        last_read_segment = sample / sps;

        /* the skew we have between running_time and the ringbuffertime
         * (last written to) */
        segment_skew = running_time_segment - last_written_segment;

        gint64 timestamp_diff = ABS(GST_CLOCK_DIFF (timestamp, base_time));
        if (!first_sample && self->initial_timestamp_diff == 0) { // second sample
          self->initial_timestamp_diff = timestamp_diff;
        }

        gint64 drift_ns = timestamp_diff > 0 ? ABS(self->initial_timestamp_diff - timestamp_diff) : 0; //nanoseconds
        if (drift_ns > self->drift_correction_threshold) {
          drift_correction = TRUE;
          self->initial_timestamp_diff = 0;
          self->drift_correction_count++;
        }

        GST_DEBUG_OBJECT (bsrc,
            "\n running_time                                               = %"
            GST_TIME_FORMAT
            "\n timestamp                                                  = %"
            GST_TIME_FORMAT
            "\n initial_timestamp_diff                                     = %"
            GST_TIME_FORMAT
            "\n timestamp_diff                                             = %"
            GST_TIME_FORMAT
            "\n drift                                                      = %"
            GST_TIME_FORMAT
            "\n running_time_segment                                       = %d"
            "\n last_written_segment                                       = %d"
            "\n segment_skew (running time segment - last_written_segment) = %d"
            "\n last_read_segment                                          = %d",
            GST_TIME_ARGS (running_time), GST_TIME_ARGS (timestamp),
            GST_TIME_ARGS (self->initial_timestamp_diff),
            GST_TIME_ARGS (timestamp_diff),
            GST_TIME_ARGS (drift_ns),
            running_time_segment, last_written_segment, segment_skew,
            last_read_segment);

        if ((segment_skew >= ringbuffer->spec.segtotal) ||
            (last_read_segment == 0) || first_sample ||
            drift_correction) {
          gint new_read_segment = running_time_segment;
          gint segment_diff;
          guint64 new_sample;

          /* the difference between running_time and the last written segment */
          segment_diff = running_time_segment - last_written_segment;

          /* advance the ringbuffer, if we need to */
          if (segment_diff != 0) {
            gst_audio_ring_buffer_advance (ringbuffer, segment_diff);

            /* we move the  new read segment to the last known written segment */
            new_read_segment =
              g_atomic_int_get (&ringbuffer->segdone) - ringbuffer->segbase;
          }

          /* we calculate the new sample value */
          new_sample = ((guint64) new_read_segment) * sps;

          /* and get the relative time to this -> our new timestamp */
          timestamp = gst_util_uint64_scale_int (new_sample, GST_SECOND, rate);

          /* we update the next sample accordingly */
          src->next_sample = new_sample + samples;

          GST_DEBUG_OBJECT (bsrc,
              "Timeshifted the ringbuffer with %d segments: "
              "Updating the timestamp to %" GST_TIME_FORMAT ", "
              "and src->next_sample to %" G_GUINT64_FORMAT, segment_diff,
              GST_TIME_ARGS (timestamp), src->next_sample);

          self->timeshifted_count++;
        }
        break;
      }
      case GST_AUDIO_BASE_SRC_SLAVE_SKEW:
      {
        GstClockTime running_time;
        GstClockTime base_time;
        GstClockTime current_time;
        guint64 running_time_sample;
        gint running_time_segment;
        gint last_read_segment;
        gint segment_skew;
        gint sps;
        gint segments_written;
        gint last_written_segment;

        /* get the amount of segments written from the device by now */
        segments_written = g_atomic_int_get (&ringbuffer->segdone);

        /* subtract the base to segments_written to get the number of the
         * last written segment in the ringbuffer
         * (one segment written = segment 0) */
        last_written_segment = segments_written - ringbuffer->segbase - 1;

        /* samples per segment */
        sps = ringbuffer->samples_per_seg;

        /* get the current time */
        current_time = gst_clock_get_time (clock);

        /* get the basetime */
        base_time = GST_ELEMENT_CAST (src)->base_time;

        /* get the running_time */
        running_time = current_time - base_time;

        /* the running_time converted to a sample
         * (relative to the ringbuffer) */
        running_time_sample =
            gst_util_uint64_scale_int (running_time, rate, GST_SECOND);

        /* the segmentnr corresponding to running_time, round down */
        running_time_segment = running_time_sample / sps;

        /* the segment currently read from the ringbuffer */
        last_read_segment = sample / sps;

        /* the skew we have between running_time and the ringbuffertime
         * (last written to) */
        segment_skew = running_time_segment - last_written_segment;

        GST_DEBUG_OBJECT (bsrc,
            "\n running_time                                              = %"
            GST_TIME_FORMAT
            "\n timestamp                                                  = %"
            GST_TIME_FORMAT
            "\n running_time_segment                                       = %d"
            "\n last_written_segment                                       = %d"
            "\n segment_skew (running time segment - last_written_segment) = %d"
            "\n last_read_segment                                          = %d",
            GST_TIME_ARGS (running_time), GST_TIME_ARGS (timestamp),
            running_time_segment, last_written_segment, segment_skew,
            last_read_segment);

        /* Resync the ringbuffer if:
         *
         * 1. We are more than the length of the ringbuffer behind.
         *    The length of the ringbuffer then gets to dictate
         *    the threshold for what is considered "too late"
         *
         * 2. If this is our first buffer.
         *    We know that we should catch up to running_time
         *    the first time we are ran.
         */
        if ((segment_skew >= ringbuffer->spec.segtotal) ||
            (last_read_segment == 0) || first_sample) {
          gint new_read_segment;
          gint segment_diff;
          guint64 new_sample;

          /* the difference between running_time and the last written segment */
          segment_diff = running_time_segment - last_written_segment;

          /* advance the ringbuffer */
          gst_audio_ring_buffer_advance (ringbuffer, segment_diff);

          /* we move the  new read segment to the last known written segment */
          new_read_segment =
              g_atomic_int_get (&ringbuffer->segdone) - ringbuffer->segbase;

          /* we calculate the new sample value */
          new_sample = ((guint64) new_read_segment) * sps;

          /* and get the relative time to this -> our new timestamp */
          timestamp = gst_util_uint64_scale_int (new_sample, GST_SECOND, rate);

          /* we update the next sample accordingly */
          src->next_sample = new_sample + samples;

          GST_DEBUG_OBJECT (bsrc,
              "Timeshifted the ringbuffer with %d segments: "
              "Updating the timestamp to %" GST_TIME_FORMAT ", "
              "and src->next_sample to %" G_GUINT64_FORMAT, segment_diff,
              GST_TIME_ARGS (timestamp), src->next_sample);

          self->timeshifted_count++;
        }
        break;
      }
      case GST_AUDIO_BASE_SRC_SLAVE_RE_TIMESTAMP:
      {
        GstClockTime base_time, latency;

        /* We are slaved to another clock. Take running time of the pipeline
         * clock and timestamp against it. Somebody else in the pipeline should
         * figure out the clock drift. We keep the duration we calculated
         * above. */
        timestamp = gst_clock_get_time (clock);
        base_time = GST_ELEMENT_CAST (src)->base_time;

        if (GST_CLOCK_DIFF (timestamp, base_time) < 0)
          timestamp -= base_time;
        else
          timestamp = 0;

        /* subtract latency */
        latency = gst_util_uint64_scale_int (total_samples, GST_SECOND, rate);
        if (timestamp > latency)
          timestamp -= latency;
        else
          timestamp = 0;
      }
      case GST_AUDIO_BASE_SRC_SLAVE_NONE:
        break;
    }
  } else {
    GstClockTime base_time;

    if (GST_CLOCK_TIME_IS_VALID (rb_timestamp)) {
      /* the read method returned a timestamp so we use this instead */
      timestamp = rb_timestamp;
    } else {
      /* to get the timestamp against the clock we also need to add our
       * offset */
      timestamp = gst_audio_clock_adjust (GST_AUDIO_CLOCK (clock), timestamp);
    }

    /* we are not slaved, subtract base_time */
    base_time = GST_ELEMENT_CAST (src)->base_time;

    if (GST_CLOCK_DIFF (timestamp, base_time) < 0) {
      timestamp -= base_time;
      GST_LOG_OBJECT (src,
          "buffer timestamp %" GST_TIME_FORMAT " (base_time %" GST_TIME_FORMAT
          ")", GST_TIME_ARGS (timestamp), GST_TIME_ARGS (base_time));
    } else {
      GST_LOG_OBJECT (src,
          "buffer timestamp 0, ts %" GST_TIME_FORMAT " <= base_time %"
          GST_TIME_FORMAT, GST_TIME_ARGS (timestamp),
          GST_TIME_ARGS (base_time));
      timestamp = 0;
    }
  }

no_sync:
  GST_OBJECT_UNLOCK (src);

  GST_BUFFER_TIMESTAMP (buf) = timestamp;
  GST_BUFFER_DURATION (buf) = duration;
  GST_BUFFER_OFFSET (buf) = sample;
  GST_BUFFER_OFFSET_END (buf) = sample + samples;

  *outbuf = buf;

  GST_LOG_OBJECT (src, "Pushed buffer timestamp %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

  return GST_FLOW_OK;

  /* ERRORS */
wrong_state:
  {
    GST_DEBUG_OBJECT (src, "ringbuffer in wrong state");
    return GST_FLOW_FLUSHING;
  }
wrong_offset:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, SEEK,
        (NULL), ("resource can only be operated on sequentially but offset %"
            G_GUINT64_FORMAT " was given", offset));
    return GST_FLOW_ERROR;
  }
alloc_failed:
  {
    GST_DEBUG_OBJECT (src, "alloc failed: %s", gst_flow_get_name (ret));
    return ret;
  }
stopped:
  {
    gst_buffer_unmap (buf, &info);
    gst_buffer_unref (buf);
    GST_DEBUG_OBJECT (src, "ringbuffer stopped");
    return GST_FLOW_FLUSHING;
  }
got_error:
  {
    gst_buffer_unmap (buf, &info);
    gst_buffer_unref (buf);
    GST_DEBUG_OBJECT (src, "ringbuffer was in error state, bailing out");
    return GST_FLOW_ERROR;
  }
}


