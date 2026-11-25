/* Block-voting ridge matcher, for fingerprints too small for minutiae.
 *
 * NBIS/MINDTCT needs a direction map, and on this sensor's 80x64 contact patch
 * it returns nothing usable -- every point it reported sat on the border of the
 * artificial padding, not on ridge structure. Vendor matchers for sensors this
 * small do not use minutiae at all: they compare ridge texture, locally.
 *
 * This tool does that offline so the idea can be measured before any driver
 * work: split the frame into overlapping blocks, high-pass each one so shading
 * and contact pressure drop out, then vote over global translations. A genuine
 * pair has many blocks agreeing on one shift; an impostor's blocks vote at
 * random. The winning vote count is the score.
 *
 *   ./tools/build-match-patch.sh
 *   ./build/match-patch captures/index/finger-*.pgm captures/other/other-*.pgm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 80
#define H 64
#define BS 16
#define STEP 8
#define MAXSHIFT 10
#define NCC_MIN 0.45f
#define MAXFILES 64

typedef struct
{
  char name[128];
  char group[32];
  float px[H][W];   /* high-passed */
  float energy[H][W];
  int   valid;
} Sample;

static Sample samples[MAXFILES];
static int nsamples;

static int
read_pgm (const char *path, unsigned char *out, int *w, int *h)
{
  FILE *f = fopen (path, "rb");
  char magic[3] = {0};
  int maxval;

  if (!f)
    return -1;
  if (fscanf (f, "%2s %d %d %d", magic, w, h, &maxval) != 4 ||
      strcmp (magic, "P5") != 0)
    {
      fclose (f);
      return -1;
    }
  fgetc (f);
  if (fread (out, 1, (size_t) (*w) * (*h), f) != (size_t) (*w) * (*h))
    {
      fclose (f);
      return -1;
    }
  fclose (f);
  return 0;
}

/* Box mean subtraction: removes the slowly varying pedestal (contact pressure,
 * LED falloff) and keeps the ridge modulation, which is what carries identity. */
static void
highpass (Sample *s)
{
  static float sum[H + 1][W + 1];
  const int r = 4;
  int x, y, dx, dy;

  for (y = 0; y <= H; y++)
    {
      sum[y][0] = 0;
      for (x = 0; x <= W; x++)
        sum[y][x] = 0;
    }

  /* prefix sums over the original values, which we stash in energy[] first */
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      s->energy[y][x] = s->px[y][x];

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      sum[y + 1][x + 1] = s->energy[y][x] + sum[y][x + 1] + sum[y + 1][x] - sum[y][x];

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      {
        int x0 = x - r < 0 ? 0 : x - r;
        int x1 = x + r >= W ? W - 1 : x + r;
        int y0 = y - r < 0 ? 0 : y - r;
        int y1 = y + r >= H ? H - 1 : y + r;
        float area = (float) ((x1 - x0 + 1) * (y1 - y0 + 1));
        float mean = (sum[y1 + 1][x1 + 1] - sum[y0][x1 + 1]
                      - sum[y1 + 1][x0] + sum[y0][x0]) / area;

        s->px[y][x] -= mean;
      }

  (void) dx;
  (void) dy;
}

/* Per-pixel activity, used to skip blocks that hold no ridge detail at all
 * (background, the area outside the contact patch). */
static void
activity (Sample *s)
{
  int x, y;
  double total = 0.0;

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      total += s->px[y][x] * s->px[y][x];

  total /= (double) (W * H);
  s->valid = sqrt (total) > 1.0;
}

static void
normalise_pixels (Sample *s)
{
  int x, y;
  double mean = 0.0, var = 0.0;

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      mean += s->px[y][x];
  mean /= (double) (W * H);

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      {
        s->px[y][x] -= (float) mean;
        var += s->px[y][x] * s->px[y][x];
      }
  var /= (double) (W * H);

  if (var < 1.0)
    {
      s->valid = 0;
      return;
    }
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      s->px[y][x] /= (float) sqrt (var);
}

/* Normalised correlation of one block of a against the same block of b shifted
 * by (dx,dy). Both blocks are normalised locally, so contact pressure and
 * brightness differences do not matter. */
static float
block_ncc (const Sample *a, const Sample *b, int bx, int by, int dx, int dy)
{
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  int x, y, n = 0;

  for (y = 0; y < BS; y++)
    {
      int ay = by + y, byy = by + y + dy;

      if (ay < 0 || ay >= H || byy < 0 || byy >= H)
        continue;
      for (x = 0; x < BS; x++)
        {
          int ax = bx + x, bxx = bx + x + dx;
          double va, vb;

          if (ax < 0 || ax >= W || bxx < 0 || bxx >= W)
            continue;
          va = a->px[ay][ax];
          vb = b->px[byy][bxx];
          sa += va;
          sb += vb;
          saa += va * va;
          sbb += vb * vb;
          sab += va * vb;
          n++;
        }
    }

  if (n < BS * BS / 2)
    return -1.0f;

  {
    double cov = sab - sa * sb / n;
    double va = saa - sa * sa / n;
    double vb = sbb - sb * sb / n;

    if (va <= 1e-6 || vb <= 1e-6)
      return -1.0f;
    return (float) (cov / sqrt (va * vb));
  }
}

/* Returns the winning vote count and writes the shift and mean correlation. */
static int
match (const Sample *a, const Sample *b, int *out_dx, int *out_dy, float *out_ncc)
{
  static int votes[2 * MAXSHIFT + 1][2 * MAXSHIFT + 1];
  static float nccsum[2 * MAXSHIFT + 1][2 * MAXSHIFT + 1];
  int dx, dy, bx, by;
  int best = 0;

  memset (votes, 0, sizeof (votes));
  memset (nccsum, 0, sizeof (nccsum));

  for (by = 0; by + BS <= H; by += STEP)
    for (bx = 0; bx + BS <= W; bx += STEP)
      {
        for (dy = -MAXSHIFT; dy <= MAXSHIFT; dy++)
          for (dx = -MAXSHIFT; dx <= MAXSHIFT; dx++)
            {
              float c = block_ncc (a, b, bx, by, dx, dy);

              if (c >= NCC_MIN)
                {
                  votes[dy + MAXSHIFT][dx + MAXSHIFT]++;
                  nccsum[dy + MAXSHIFT][dx + MAXSHIFT] += c;
                }
            }
      }

  for (dy = 0; dy <= 2 * MAXSHIFT; dy++)
    for (dx = 0; dx <= 2 * MAXSHIFT; dx++)
      if (votes[dy][dx] > best)
        {
          best = votes[dy][dx];
          *out_dx = dx - MAXSHIFT;
          *out_dy = dy - MAXSHIFT;
          *out_ncc = nccsum[dy][dx] / votes[dy][dx];
        }
  if (best == 0)
    *out_ncc = 0.0f;
  return best;
}

static void
derive_group (Sample *s)
{
  const char *base = strrchr (s->name, '/');
  const char *dot;
  int n;

  base = base ? base + 1 : s->name;
  n = (int) strlen (base);
  /* "<group>-<nn>.pgm" */
  dot = strchr (base, '-');
  if (dot)
    {
      int len = (int) (dot - base);

      if (len > 31)
        len = 31;
      memcpy (s->group, base, (size_t) len);
      s->group[len] = 0;
    }
  else
    {
      snprintf (s->group, sizeof (s->group), "%s", base);
      if (n > 4)
        s->group[n - 4] = 0;
    }
}

int
main (int argc, char **argv)
{
  static int score[MAXFILES][MAXFILES];
  static float nccv[MAXFILES][MAXFILES];
  char groups[MAXFILES][32];
  int ngroups = 0;
  int genuine_floor = 1 << 30, impostor_ceil = 0;
  int i, j, g;

  setvbuf (stdout, NULL, _IOLBF, 0);

  if (argc < 3)
    {
      fprintf (stderr, "usage: %s <file.pgm> <file.pgm> [...]\n", argv[0]);
      return 2;
    }

  for (i = 1; i < argc && nsamples < MAXFILES; i++)
    {
      static unsigned char raw[W * H];
      int w, h;

      if (read_pgm (argv[i], raw, &w, &h) < 0 || w != W || h != H)
        {
          fprintf (stderr, "%s: expected %dx%d PGM\n", argv[i], W, H);
          continue;
        }
      snprintf (samples[nsamples].name, sizeof (samples[0].name), "%s", argv[i]);
      derive_group (&samples[nsamples]);
      for (h = 0; h < H; h++)
        for (w = 0; w < W; w++)
          samples[nsamples].px[h][w] = raw[h * W + w];
      samples[nsamples].valid = 1;
      nsamples++;
    }

  for (i = 0; i < nsamples; i++)
    {
      highpass (&samples[i]);
      activity (&samples[i]);
      normalise_pixels (&samples[i]);
    }

  printf ("%-28s %-10s\n", "file", "group");
  for (i = 0; i < nsamples; i++)
    printf ("%-28s %-10s%s\n", samples[i].name, samples[i].group,
            samples[i].valid ? "" : "  (flat)");

  for (i = 0; i < nsamples; i++)
    for (j = i + 1; j < nsamples; j++)
      {
        int dx = 0, dy = 0;
        float nc = 0.0f;
        int v = match (&samples[i], &samples[j], &dx, &dy, &nc);

        score[i][j] = score[j][i] = v;
        nccv[i][j] = nccv[j][i] = nc;
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

  printf ("\npair scores (blocks voting for one shift; max %d)\n%-24s", (W / STEP) * (H / STEP), "pair");
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

  printf ("\nbest score per capture\n%-24s", "file");
  for (g = 0; g < ngroups; g++)
    printf (" %-8s", groups[g]);
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

  if (ngroups >= 2)
    {
      printf ("\ngenuine  worst best-of-group  %d\n", genuine_floor);
      printf ("impostor best-of-group       %d\n", impostor_ceil);
      if (impostor_ceil >= genuine_floor)
        printf ("\nOVERLAP: block voting does not separate these captures\n");
      else
        printf ("\nusable threshold %d..%d (%d blocks of margin)\n",
                impostor_ceil + 1, genuine_floor,
                genuine_floor - impostor_ceil);
    }
  return 0;
}
