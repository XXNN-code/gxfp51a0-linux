/* Score stored captures with the real NBIS matcher, offline.
 *
 * The device path can only be exercised one verify at a time, as root, via
 * fprintd -- which makes threshold tuning impossible: every data point costs
 * an interactive run and only covers one gallery entry. This tool rebuilds the
 * exact same pipeline locally (pad to 128x128, MINDTCT, xyt conversion,
 * Bozorth3) and prints the full pairwise score matrix, so genuine and impostor
 * scores can be read off a single batch of captures.
 *
 *   ./tools/build-test-match.sh
 *   ./build/test-match /tmp/cap/index-0*.pgm /tmp/cap/middle-0*.pgm
 *
 * File names are treated as labels: everything up to the root name is the
 * finger (see group_of()). Pairs inside a group are genuine, across groups are
 * impostor.
 */
#include <fprint.h>
#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bozorth.h"
#include "fpi-minutiae.h"

#define CANVAS 128
#define MAX_FILES 32

/* lfs2nist_minutia_XYT() from nbis/mindtct/xytreps.c, inlined: it is not
 * exported, and the conversion is five lines. */
#define NUM_DIRECTIONS 16
#define SROUND(x) ((int) (((x) < 0) ? (x) - 0.5 : (x) + 0.5))

typedef struct
{
  char name[256];
  char group[64];
  int nmin;
  struct xyt_struct xyt;
} Sample;

static Sample samples[MAX_FILES];
static int nsamples;

static guchar *
read_pgm (const char *path, int *w, int *h)
{
  FILE *f = fopen (path, "rb");
  char magic[3] = {0};
  int maxval = 0;
  guchar *px;

  if (!f)
    return NULL;
  if (fscanf (f, "%2s %d %d %d", magic, w, h, &maxval) != 4 ||
      strcmp (magic, "P5") != 0)
    {
      fclose (f);
      return NULL;
    }
  fgetc (f);
  px = g_malloc ((size_t) *w * *h);
  if (fread (px, 1, (size_t) *w * *h, f) != (size_t) *w * *h)
    {
      g_free (px);
      px = NULL;
    }
  fclose (f);
  return px;
}

/* Same framing the driver applies before handing the image to libfprint:
 * centre on a CANVAS square filled with the frame mean. Without the margin
 * MINDTCT culls every border block and reports no minutiae at all. */
static guchar *
pad_to_canvas (const guchar *src, int w, int h)
{
  guchar *dst = g_malloc (CANVAS * CANVAS);
  int64_t sum = 0;
  int ox, oy;

  for (int i = 0; i < w * h; i++)
    sum += src[i];
  memset (dst, (int) (sum / (w * h)), CANVAS * CANVAS);

  ox = (CANVAS - w) / 2;
  oy = (CANVAS - h) / 2;
  for (int y = 0; y < h; y++)
    memcpy (dst + (oy + y) * CANVAS + ox, src + y * w, (size_t) w);
  return dst;
}

static int
my_sort_x_y (const void *a, const void *b)
{
  const int *p = a, *q = b;

  if (p[0] != q[0])
    return p[0] - q[0];
  return p[1] - q[1];
}

static void
to_xyt (GPtrArray *minutiae, int width, int height, struct xyt_struct *xyt)
{
  int c[MAX_FILE_MINUTIAE][4];
  int nmin = minutiae ? MIN ((int) minutiae->len, MAX_BOZORTH_MINUTIAE) : 0;

  for (int i = 0; i < nmin; i++)
    {
      struct fp_minutia *m = g_ptr_array_index (minutiae, i);
      int t = (270 - SROUND (m->direction * 180.0 / NUM_DIRECTIONS)) % 360;

      if (t < 0)
        t += 360;
      c[i][0] = m->x;
      c[i][1] = height - m->y;
      c[i][2] = t > 180 ? t - 360 : t;
      c[i][3] = 0;
    }

  qsort (c, (size_t) nmin, sizeof (c[0]), my_sort_x_y);
  for (int i = 0; i < nmin; i++)
    {
      xyt->xcol[i] = c[i][0];
      xyt->ycol[i] = c[i][1];
      xyt->thetacol[i] = c[i][2];
    }
  xyt->nrows = nmin;
}

typedef struct
{
  Sample    *sample;
  GMainLoop *loop;
} DetectJob;

static void
on_minutiae (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GError *error = NULL;
  GPtrArray *minutiae = NULL;
  DetectJob *job = user_data;
  Sample *s = job->sample;

  fp_image_detect_minutiae_finish (FP_IMAGE (src), res, &error);
  if (error)
    {
      fprintf (stderr, "%s: %s\n", s->name, error->message);
      g_clear_error (&error);
    }
  else
    {
      minutiae = fp_image_get_minutiae (FP_IMAGE (src));
    }
  to_xyt (minutiae, CANVAS, CANVAS, &s->xyt);
  s->nmin = s->xyt.nrows;
  g_main_loop_quit (job->loop);
}

/* "index-03.pgm" -> "index" */
static void
group_of (const char *name, char *out)
{
  const char *base = strrchr (name, '/');
  const char *dash;
  size_t n;

  base = base ? base + 1 : name;
  dash = strrchr (base, '-');
  n = dash ? (size_t) (dash - base) : strlen (base);
  if (n >= 63)
    n = 63;
  memcpy (out, base, n);
  out[n] = '\0';
}

static int
load (const char *path, Sample *s)
{
  guchar *px, *canvas;
  int w = 0, h = 0;
  g_autoptr(FpImage) img = NULL;
  GMainLoop *loop;
  DetectJob job;
  gsize len = 0;
  guchar *dst;

  px = read_pgm (path, &w, &h);
  if (!px)
    {
      fprintf (stderr, "%s: cannot read\n", path);
      return -1;
    }
  snprintf (s->name, sizeof (s->name), "%s", path);
  group_of (path, s->group);

  canvas = pad_to_canvas (px, w, h);
  g_free (px);

  img = fp_image_new (CANVAS, CANVAS);
  dst = (guchar *) fp_image_get_data (img, &len);
  memcpy (dst, canvas, len);
  g_free (canvas);

  loop = g_main_loop_new (NULL, FALSE);
  job.sample = s;
  job.loop = loop;
  fp_image_detect_minutiae (img, NULL, on_minutiae, &job);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
  return 0;
}

int
main (int argc, char **argv)
{
  static int score[MAX_FILES][MAX_FILES];
  char groups[MAX_FILES][64];
  int ngroups = 0;
  int genuine_floor = 1000, impostor_ceil = 0;
  int i, j, g;

  setvbuf (stdout, NULL, _IOLBF, 0);

  if (argc < 3)
    {
      fprintf (stderr,
               "usage: %s <file.pgm> <file.pgm> [...]\n"
               "  name files <finger>-<n>.pgm so pairs can be labelled\n",
               argv[0]);
      return 2;
    }

  for (i = 1; i < argc && nsamples < MAX_FILES; i++)
    if (load (argv[i], &samples[nsamples]) == 0)
      nsamples++;

  if (nsamples < 2)
    {
      fprintf (stderr, "need at least two readable captures\n");
      return 1;
    }

  printf ("\n%-24s %-10s %s\n", "file", "finger", "minutiae");
  for (i = 0; i < nsamples; i++)
    printf ("%-24s %-10s %d\n", samples[i].name, samples[i].group,
            samples[i].nmin);

  for (i = 0; i < nsamples; i++)
    for (j = i + 1; j < nsamples; j++)
      {
        int len = bozorth_probe_init (&samples[i].xyt);

        score[i][j] = bozorth_to_gallery (len, &samples[i].xyt,
                                         &samples[j].xyt);
        score[j][i] = score[i][j];
      }

  for (i = 0; i < nsamples; i++)
    {
      for (g = 0; g < ngroups; g++)
        if (strcmp (groups[g], samples[i].group) == 0)
          break;
      if (g == ngroups)
        snprintf (groups[ngroups++], sizeof (groups[0]), "%s",
                  samples[i].group);
    }

  printf ("\n%-24s", "pair");
  for (j = 0; j < nsamples; j++)
    printf (" %3d", j);
  printf ("\n");
  for (i = 0; i < nsamples; i++)
    {
      printf ("%-24s", samples[i].name);
      for (j = 0; j < nsamples; j++)
        {
          if (j == i)
            printf ("   -");
          else if (j < i)
            printf ("    ");
          else
            printf (" %3d", score[i][j]);
        }
      printf ("\n");
    }

  /* A verify succeeds if any single template entry matches, so the number that
   * matters is the best score against the other finger's frames, not the mean
   * over pairs. */
  printf ("\nbest score per capture (left column = its own finger)\n");
  printf ("%-24s", "file");
  for (g = 0; g < ngroups; g++)
    printf (" %-8s", groups[g][0] ? groups[g] : "?");
  printf ("\n");

  for (i = 0; i < nsamples; i++)
    {
      int own = -1;

      printf ("%-24s", samples[i].name);
      for (g = 0; g < ngroups; g++)
        {
          int best = -1;

          for (j = 0; j < nsamples; j++)
            {
              if (j == i || strcmp (samples[j].group, groups[g]) != 0)
                continue;
              if (best < 0 || score[i][j] > best)
                best = score[i][j];
            }
          printf (" %-8d", best);
          if (strcmp (samples[i].group, groups[g]) == 0)
            own = best;
        }
      printf ("\n");
      if (ngroups > 1 && own >= 0)
        {
          if (own < genuine_floor)
            genuine_floor = own;
          for (g = 0; g < ngroups; g++)
            {
              int best = -1;

              if (strcmp (samples[i].group, groups[g]) == 0)
                continue;
              for (j = 0; j < nsamples; j++)
                {
                  if (j == i || strcmp (samples[j].group, groups[g]) != 0)
                    continue;
                  if (best < 0 || score[i][j] > best)
                    best = score[i][j];
                }
              if (best > impostor_ceil)
                impostor_ceil = best;
            }
        }
    }

  /* libfprint accepts on the first entry that clears the threshold, so one
   * lucky pairing is enough. Counting how many entries each probe clears shows
   * whether a "N of M entries" rule would separate better than the threshold
   * alone. */
  printf ("\nentries cleared, per probe\n");
  printf ("%-24s", "file");
  for (g = 0; g < 3; g++)
    printf ("  >=%d same/cross", (g + 1) * 4);
  printf ("\n");
  for (i = 0; i < nsamples; i++)
    {
      printf ("%-24s", samples[i].name);
      for (g = 0; g < 3; g++)
        {
          int th = (g + 1) * 4;
          int same = 0, cross = 0;

          for (j = 0; j < nsamples; j++)
            {
              if (j == i || score[i][j] < th)
                continue;
              if (strcmp (samples[i].group, samples[j].group) == 0)
                same++;
              else
                cross++;
            }
          printf ("      %2d / %-5d", same, cross);
        }
      printf ("\n");
    }

  if (ngroups < 2)
    {
      printf ("\nnote: only one finger group, no impostor data\n");
      return 0;
    }

  printf ("\ngenuine  worst best-of-template score %d\n", genuine_floor);
  printf ("impostor best-of-template score       %d\n", impostor_ceil);
  if (impostor_ceil >= genuine_floor)
    printf ("\nOVERLAP: best-of-template does not separate these captures\n");
  else
    printf ("\nusable threshold range %d..%d (%d point%s of margin)\n",
            impostor_ceil + 1, genuine_floor,
            genuine_floor - impostor_ceil,
            genuine_floor - impostor_ceil == 1 ? "" : "s");
  return 0;
}
