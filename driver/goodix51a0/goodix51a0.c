/*
 * Goodix GXFP51A0 (GF3658 / ST411) SPI fingerprint sensor
 *
 * The sensor sits on spidev with a GPIO reset line and a GPIO interrupt line,
 * runs a TLS-PSK tunnel with the host as server, and returns a 80x64 12-bit
 * image inside that tunnel. All of that protocol work lives in gx51/session.c
 * and its helpers, which are the same sources tools/gx51-probe is built from.
 *
 * Threading: libfprint calls the image-device vfuncs on the main context, but
 * a full capture session is seconds of blocking SPI traffic (reset, F2 PMK
 * extraction, target configure, handshake, image transfer). So activate()
 * starts a worker thread that owns the session and the main thread only ever
 * sees marshalled calls back through g_main_context_invoke().
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define FP_COMPONENT "goodix51a0"

#include "drivers_api.h"
#include "fpi-image-device.h"
#include "fpi-image.h"

#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define GX51_IMG_W GX51_IMAGE_WIDTH
#define GX51_IMG_H GX51_IMAGE_HEIGHT

/* Native resolution: a 9 px ridge period is ~0.5 mm, i.e. 500 dpi. */
#define GX51_PPMM 19.69

/* An idle sensor reads a mean of ~350 per zone and a pressed one ~185, with
 * touchflag 0x3f lighting up all six zones. Five of six is the vendor's
 * threshold and it is what the probe validates on this unit. */
#define GX51_TOUCH_ZONES 5
#define GX51_MIN_RANGE 64
/* How long a single blocking poll may run before the worker checks for
 * cancellation again. */
#define GX51_POLL_SLICE_MS 200

G_DECLARE_FINAL_TYPE (FpiDeviceGoodix51a0,
                      fpi_device_goodix51a0,
                      FPI,
                      DEVICE_GOODIX51A0,
                      FpImageDevice)

struct _FpiDeviceGoodix51a0
{
  FpImageDevice parent;

  gchar *spi_path;
  struct gx51_session *session;
  GThread *thread;
  gint cancel;
};

G_DEFINE_TYPE (FpiDeviceGoodix51a0, fpi_device_goodix51a0, FP_TYPE_IMAGE_DEVICE);

/* ------------------------------------------------------- main-thread glue */

typedef struct
{
  FpImageDevice *dev;
  gboolean       present;
} FingerMsg;

static gboolean
deliver_finger_status (gpointer data)
{
  FingerMsg *msg = data;

  fpi_image_device_report_finger_status (msg->dev, msg->present);
  g_free (msg);
  return G_SOURCE_REMOVE;
}

static void
report_finger (FpImageDevice *dev, gboolean present)
{
  FingerMsg *msg = g_new0 (FingerMsg, 1);

  msg->dev = dev;
  msg->present = present;
  g_main_context_invoke (NULL, deliver_finger_status, msg);
}

typedef struct
{
  FpImageDevice *dev;
  FpImage       *image;
} ImageMsg;

static gboolean
deliver_image (gpointer data)
{
  ImageMsg *msg = data;

  fpi_image_device_image_captured (msg->dev, msg->image);
  g_free (msg);
  return G_SOURCE_REMOVE;
}

typedef struct
{
  FpImageDevice *dev;
  GError        *error;
} ActivateMsg;

static gboolean
deliver_activate (gpointer data)
{
  ActivateMsg *msg = data;

  if (msg->error != NULL)
    fprintf (stderr, "gx51: activation failed: %s\n", msg->error->message);

  /* activate() holds a critical section so that a cancel cannot reach the
   * driver while the session is still being set up; release it before
   * reporting completion, as required for command completion. */
  fpi_device_critical_leave (FP_DEVICE (msg->dev));
  /* activate_complete() takes ownership of the error (it lands in the current
   * action); clearing it here would free it twice. */
  fpi_image_device_activate_complete (msg->dev, msg->error);
  g_free (msg);
  return G_SOURCE_REMOVE;
}

static void
report_activate (FpImageDevice *dev, GError *error)
{
  ActivateMsg *msg = g_new0 (ActivateMsg, 1);

  msg->dev = dev;
  msg->error = error;
  g_main_context_invoke (NULL, deliver_activate, msg);
}

/* ------------------------------------------------------------- worker side */

static gboolean
cancelled (FpiDeviceGoodix51a0 *self)
{
  return g_atomic_int_get (&self->cancel) != 0;
}

/* Wait for the finger, in slices, so deactivate() stays responsive. */
static gboolean
wait_for_finger (FpiDeviceGoodix51a0 *self, int *mean)
{
  while (!cancelled (self)) {
    if (gx51_session_wait_touch (self->session, GX51_POLL_SLICE_MS,
                                 GX51_TOUCH_ZONES, mean) == 0)
      return TRUE;
  }
  return FALSE;
}

/* Wait for the finger to leave. Gives up after `timeout_ms` but keeps
 * checking for cancellation; a still-present finger is not an error, the next
 * capture simply fails its range check. */
static gboolean
wait_for_lift (FpiDeviceGoodix51a0 *self, int timeout_ms)
{
  gint64 deadline = g_get_monotonic_time () + (gint64)timeout_ms * 1000;

  while (!cancelled (self) && g_get_monotonic_time () < deadline) {
    if (gx51_session_wait_clear (self->session, GX51_POLL_SLICE_MS) == 0)
      return TRUE;
  }
  return FALSE;
}

/* 12-bit samples to 8-bit, stretched on the 1st/99th percentile so the ridge
 * modulation fills the byte instead of the background pedestal. */
static void
samples_to_gray (const uint16_t *px, guchar *out)
{
  struct gx51_image_stats st;
  guint lo;
  guint hi;
  gsize i;

  gx51_image_stats (px, GX51_IMAGE_PIXELS, &st);
  lo = st.p1;
  hi = st.p99;
  if (hi <= lo) {
      lo = st.min;
      hi = st.max;
  }

  for (i = 0; i < GX51_IMAGE_PIXELS; i++) {
      guint v = px[i] & 0x0fffu;

      if (hi <= lo)
        out[i] = 0;
      else if (v <= lo)
        out[i] = 0;
      else if (v >= hi)
        out[i] = 255;
      else
        out[i] = (guchar) ((v - lo) * 255u / (hi - lo));
  }
}

/* MINDTCT works on a block grid (8 px blocks, 24 px direction window) and
 * culls blocks that touch the image border. At the native 80x64 the ridge area
 * fills the whole frame, so after culling too few valid blocks remain and every
 * single capture ends in "No minutiae found". Centring the frame on a
 * background-filled 128x128 canvas gives the culling something to eat: measured
 * over 10 stored captures it goes from 0/10 detections to 10/10, at ~8 minutiae
 * per frame. */
#define GX51_CANVAS 128u

static void
push_image (FpiDeviceGoodix51a0 *self, const uint16_t *px)
{
  g_autoptr(FpImage) image = fp_image_new (GX51_CANVAS, GX51_CANVAS);
  ImageMsg *msg;
  guchar gray[GX51_IMAGE_PIXELS];
  guint64 sum = 0;
  guchar bg;
  gint ox, oy;
  gsize i;
  guint y;

  if (image == NULL) {
      fp_warn ("could not allocate the image");
      return;
  }
  samples_to_gray (px, gray);
  for (i = 0; i < GX51_IMAGE_PIXELS; i++)
      sum += gray[i];
  bg = (guchar) (sum / GX51_IMAGE_PIXELS);

  memset (image->data, bg, (gsize) GX51_CANVAS * GX51_CANVAS);
  ox = (gint) ((GX51_CANVAS - GX51_IMG_W) / 2u);
  oy = (gint) ((GX51_CANVAS - GX51_IMG_H) / 2u);
  for (y = 0; y < GX51_IMG_H; y++)
      memcpy (image->data + (gsize) (oy + y) * GX51_CANVAS + ox,
              gray + (gsize) y * GX51_IMG_W, GX51_IMG_W);

  /* Radius of the reliability neighbourhood is derived from this. */
  image->ppmm = GX51_PPMM;

  msg = g_new0 (ImageMsg, 1);
  msg->dev = FP_IMAGE_DEVICE (self);
  msg->image = g_steal_pointer (&image);
  g_main_context_invoke (NULL, deliver_image, msg);
}

static gint
frame_range (const uint16_t *px)
{
  struct gx51_image_stats st;

  gx51_image_stats (px, GX51_IMAGE_PIXELS, &st);
  return (gint) st.max - (gint) st.min;
}

static gpointer
capture_worker (gpointer data)
{
  FpiDeviceGoodix51a0 *self = data;
  FpImageDevice *dev = FP_IMAGE_DEVICE (self);
  struct gx51_session *session;
  uint16_t *px;
  gchar message[256];

  session = g_new0 (struct gx51_session, 1);
  session->verbose = TRUE;
  self->session = session;

  px = g_new0 (uint16_t, GX51_IMAGE_PIXELS);

  /* Open + reset + DriverState:Install + firmware check. */
  fprintf (stderr, "gx51: activating on %s\n", self->spi_path);
  if (gx51_session_open (session, self->spi_path) < 0) {
      g_snprintf (message, sizeof (message),
                  "could not open the sensor: %s", g_strerror (errno));
      report_activate (dev, g_error_new_literal (FP_DEVICE_ERROR,
                                                 FP_DEVICE_ERROR_GENERAL,
                                                 message));
      goto out;
  }
  fprintf (stderr, "gx51: sensor firmware %s\n", session->version);

  /* PMK, post-staging reset, target configure, REQTLS, handshake, D4 wakeup. */
  fprintf (stderr, "gx51: opened, establishing the TLS session\n");
  if (gx51_session_start_tls (session) < 0) {
      report_activate (dev, g_error_new_literal (FP_DEVICE_ERROR,
                                                 FP_DEVICE_ERROR_GENERAL,
                                                 "could not establish the TLS session"));
      goto out;
  }

  /* The sensor must prove it is unoccupied before ImageBase is built. */
  wait_for_lift (self, 3000);
  fprintf (stderr, "gx51: tunnel up, building the background reference\n");
  if (gx51_session_background (session) < 0) {
      report_activate (dev, g_error_new_literal (FP_DEVICE_ERROR,
                                                 FP_DEVICE_ERROR_GENERAL,
                                                 "background reference failed"));
      goto out;
  }

  report_activate (dev, NULL);
  fprintf (stderr, "gx51: session up, waiting for a finger\n");

  while (!cancelled (self)) {
      gint mean = 0;
      gint range;

      if (!wait_for_finger (self, &mean))
        break;

      fprintf (stderr, "gx51: finger down (zone mean %d), capturing\n", mean);
      report_finger (dev, TRUE);

      if (gx51_session_capture (session, px) < 0) {
          fp_warn ("capture failed, retrying");
          report_finger (dev, FALSE);
          gx51_session_cleanup (session);
          continue;
      }

      range = frame_range (px);
      if (range < GX51_MIN_RANGE) {
          fp_warn ("frame too flat (range %d), retrying", range);
          report_finger (dev, FALSE);
          wait_for_lift (self, 3000);
          gx51_session_cleanup (session);
          continue;
      }

      fprintf (stderr, "gx51: captured frame, range %d\n", range);
      push_image (self, px);
      /* Report the lift before waiting for it: the image device only leaves
       * await-finger-off once it sees this, and a verifier waiting on the
       * action should not sit through the whole 5 s poll window. */
      report_finger (dev, FALSE);
      wait_for_lift (self, 5000);
      gx51_session_cleanup (session);
  }

out:
  g_free (px);
  return NULL;
}

/* ------------------------------------------------------------- vfuncs */

static void
goodix51a0_img_open (FpImageDevice *dev)
{
  FpiDeviceGoodix51a0 *self = FPI_DEVICE_GOODIX51A0 (dev);
  const gchar *spi_path;

  spi_path = fpi_device_get_udev_data (FP_DEVICE (dev),
                                       FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);
  if (spi_path == NULL) {
      fpi_image_device_open_complete (dev,
                                      g_error_new_literal (FP_DEVICE_ERROR,
                                                           FP_DEVICE_ERROR_GENERAL,
                                                           "no spidev node"));
      return;
  }
  self->spi_path = g_strdup (spi_path);
  fpi_image_device_open_complete (dev, NULL);
}

static void
goodix51a0_img_close (FpImageDevice *dev)
{
  FpiDeviceGoodix51a0 *self = FPI_DEVICE_GOODIX51A0 (dev);

  g_clear_pointer (&self->spi_path, g_free);
  fpi_image_device_close_complete (dev, NULL);
}

static void
goodix51a0_activate (FpImageDevice *dev)
{
  FpiDeviceGoodix51a0 *self = FPI_DEVICE_GOODIX51A0 (dev);

  g_atomic_int_set (&self->cancel, 0);
  /* The setup sequence takes seconds; without this a cancel arriving mid-way
   * would run deactivate() while activation is still in progress, which the
   * state machine does not allow. */
  fpi_device_critical_enter (FP_DEVICE (dev));
  self->thread = g_thread_new ("gx51-capture", capture_worker, self);
}

static void
goodix51a0_deactivate (FpImageDevice *dev)
{
  FpiDeviceGoodix51a0 *self = FPI_DEVICE_GOODIX51A0 (dev);

  g_atomic_int_set (&self->cancel, 1);
  if (self->thread != NULL) {
      g_thread_join (self->thread);
      self->thread = NULL;
  }
  if (self->session != NULL) {
      gx51_session_close (self->session);
      g_clear_pointer (&self->session, g_free);
  }
  fpi_image_device_deactivate_complete (dev, NULL);
}

static void
fpi_device_goodix51a0_init (FpiDeviceGoodix51a0 *self)
{
  self->session = NULL;
  self->thread = NULL;
  self->spi_path = NULL;

  /* The session trace goes to stdout, which under fprintd is a pipe into the
   * journal and therefore fully buffered: without this nothing shows up until
   * 4 KiB have accumulated, which makes a stalled enrollment look like a
   * silent hang. */
  setvbuf (stdout, NULL, _IOLBF, 0);
}

static void
fpi_device_goodix51a0_finalize (GObject *object)
{
  FpiDeviceGoodix51a0 *self = FPI_DEVICE_GOODIX51A0 (object);

  g_clear_pointer (&self->spi_path, g_free);
  g_clear_pointer (&self->session, g_free);

  G_OBJECT_CLASS (fpi_device_goodix51a0_parent_class)->finalize (object);
}

static const FpIdEntry goodix51a0_id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV,
    .spi_acpi_id = "GXFP51A0" },
  { .udev_types = 0 },
};

static void
fpi_device_goodix51a0_class_init (FpiDeviceGoodix51a0Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "goodix51a0";
  dev_class->full_name = "Goodix GXFP51A0 (GF3658 / ST411)";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = goodix51a0_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  /* Each accepted frame carries only a handful of minutiae, and a template is
   * the union of one xyt set per stage, so more stages means more entries for
   * the matcher to find a consistent pair in: verification scores the best
   * entry, not the average. Offline scoring of real captures shows the same
   * finger peaking at 20 against <=8 for a finger that was never enrolled, and
   * individual frames are weak, so the extra coverage is worth the presses.
   */
  dev_class->nr_enroll_stages = 10;
  dev_class->temp_hot_seconds = -1;

  img_class->img_width = GX51_CANVAS;
  img_class->img_height = GX51_CANVAS;
  /* Bozorth3 scores cardinality, and this sensor yields 8-10 minutiae per
   * frame, so genuine pairs land at 5-20 instead of the 40-200 a full-finger
   * 500 dpi print reaches. tools/test-match.c measured, on real captures,
   * best-of-template genuine 9-20 against impostor <=8; pre-processing
   * variants (blur, local contrast, 2x/3x resampling) do not lift the count,
   * so the ceiling is the sensor's 4.0x3.2 mm window, not the pipeline.
   * 10 sits above every impostor score seen and below 9 of 10 genuine ones. */
  img_class->bz3_threshold = 10;

  img_class->img_open = goodix51a0_img_open;
  img_class->img_close = goodix51a0_img_close;
  img_class->activate = goodix51a0_activate;
  img_class->deactivate = goodix51a0_deactivate;

  G_OBJECT_CLASS (klass)->finalize = fpi_device_goodix51a0_finalize;
}
