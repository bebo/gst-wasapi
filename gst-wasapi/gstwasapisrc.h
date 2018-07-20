/*
 * Copyright (C) 2008 Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>
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

#ifndef __GST_WASAPI_SRC_H__
#define __GST_WASAPI_SRC_H__

#include "gstwasapiutil.h"

G_BEGIN_DECLS
#define GST_TYPE_WASAPI_SRC \
  (gst_wasapi_src_get_type ())
#define GST_WASAPI_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_WASAPI_SRC, GstWasapiSrc))
#define GST_WASAPI_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_WASAPI_SRC, GstWasapiSrcClass))
#define GST_IS_WASAPI_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_WASAPI_SRC))
#define GST_IS_WASAPI_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_WASAPI_SRC))
typedef struct _GstWasapiSrc GstWasapiSrc;
typedef struct _GstWasapiSrcClass GstWasapiSrcClass;

typedef struct change_notify {
  IMMNotificationClient client; // this must be first in the structure!
  IMMDeviceEnumerator *pEnumerator; // object where client is registered
  LPWSTR monitored; // Monitored device
  guint default_changed;
} change_notify;

struct _GstWasapiSrc
{
  GstAudioSrc parent;

  IMMDevice *device;
  IAudioClient *client;
  IAudioClock *client_clock;
  IMMNotificationClient notification_client;
  guint64 client_clock_freq;
  IAudioCaptureClient *capture_client;
  HANDLE event_handle;
  HANDLE stop_handle;
  HANDLE thread_priority_handle;

  gsize overflow_buffer_size;
  guint overflow_buffer_ptr;
  guint overflow_buffer_length;
  guint8 * overflow_buffer;
  /* Client was reset, so it needs to be started again */
  gboolean client_needs_restart;

  /* Actual size of the allocated buffer */
  guint buffer_frame_count;
  /* The mix format that wasapi prefers in shared mode */
  WAVEFORMATEX *mix_format;
  /* The probed caps that we can accept */
  GstCaps *cached_caps;
  /* The channel positions in the data read from the device
   * we will pass this to GstAudioRingbuffer so it can
   * translate it to the native GStreamer channel layout. */
  GstAudioChannelPosition *positions;

  // Default Audio Device changed
  change_notify change;
  gint change_initialized;
  gboolean eos_sent;

  /* properties */
  gint role;
  gint sharemode;
  gboolean loopback;
  gboolean low_latency;
  gboolean try_audioclient3;
  gint sample_rate;
  wchar_t *device_strid;
  gchar *device_description;

  guint64 capture_too_many_frames_log_count;
};

struct _GstWasapiSrcClass
{
  GstAudioSrcClass parent_class;
};

GType gst_wasapi_src_get_type (void);

G_END_DECLS
#endif /* __GST_WASAPI_SRC_H__ */
