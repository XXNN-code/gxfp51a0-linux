/* Offline test of ridge-pattern correlation matching.
 *
 * Bozorth3 needs roughly 30 minutiae to separate fingers, and this sensor's
 * 64x80 window only yields about ten, so minutiae topology cannot tell a
 * genuine press from an impostor one (see STATUS.md 7.12). Pattern correlation
 * is the other classic matcher for small sensors: it compares the ridge texture
 * itself instead of point triples. This tool measures whether that would work
 * here before any of it is wired into the driver.
 *
 *   gcc -O2 -o build/match-corr tools/match-corr.c -lm
 *   ./build/match-corr captures/index/finger-*.pgm captures/other/other-*.pgm
 *
 * Output is the same genuine/impostor table as test-match, with scores scaled
 * to 0-1000 for readability.
 */
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 80
#define H 64
#define MAX_FILES 32

typedef struct
{
  char name[256];
  char group[64];
  float px[H][W];
} Sample;

static Sample samples[MAX_FILES];
static int nsamples;
static int use_highpass;

static int
read_pgm (const char *path, int *w, int *h, unsigned char *out)
{
  FILE *f = fopen (path, "rb");
  char magic[3] = {0};
  int maxval = 0;

  if (!f)
    return -1;
  if (fscanf (f, "%2s %d %d %d", magic, w, h, &maxval) != 4 ||
      strcmp (magic, "P5") != 0)
    {
      fclose (f);
      return -1;
    }
  fgetc (f);
  if (fread (out, 1, (size_t) *w * *h, f) != (size_t) *w * *h)
    {
      fclose (f);
      return -1;
    }
  fclose (f);
  return 0;
}

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

/* Remove the slowly varying component so the comparison keys on ridge detail
 * (endings, bifurcations, scars) instead of the parallel flow, which every
 * finger in a 4 mm window shares and which alone correlates at 0.6-0.8. */
static void
highpass (float px[H][W])
{
  static float tmp[H][W];
  const int r = 2;
  int x, y, dx, dy;

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      {
        double sum = 0;
        int n = 0;

        for (dy = -r; dy <= r; dy++)
          for (dx = -r; dx <= r; dx++)
            {
              int sx = x + dx, sy = y + dy;

              if (sx < 0 || sy < 0 || sx >= W || sy >= H)
                continue;
              sum += px[sy][sx];
              n++;
            }
        tmp[y][x] = px[y][x] - (float) (sum / n);
      }
  memcpy (px, tmp, sizeof (tmp));
}

/* Zero mean, unit variance: correlation must not depend on how bright the press
 * was, only on the ridge pattern. */
static void
normalise (float px[H][W])
{
  double sum = 0, var = 0;
  int i;

  for (i = 0; i < W * H; i++)
    sum += ((float *) px)[i];
  sum /= W * H;
  for (i = 0; i < W * H; i++)
    var += (((float *) px)[i] - sum) * (((float *) px)[i] - sum);
  var = sqrt (var / (W * H));
  if (var < 1e-6)
    var = 1.0;
  for (i = 0; i < W * H; i++)
    ((float *) px)[i] = (float) ((((float *) px)[i] - sum) / var);
}

/* Bilinear rotation about the centre. Pixels that fall outside the source are
 * flagged so they do not drag the score down. */
static void
rotate (const float src[H][W], float dst[H][W], unsigned char mask[H][W],
        double deg)
{
  const double cx = (W - 1) / 2.0, cy = (H - 1) / 2.0;
  const double rad = deg * M_PI / 180.0;
  const double c = cos (rad), s = sin (rad);
  int x, y;

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      {
        double dx = x - cx, dy = y - cy;
        double sx = c * dx + s * dy + cx;
        double sy = -s * dx + c * dy + cy;
        int x0 = (int) floor (sx), y0 = (int) floor (sy);
        double fx = sx - x0, fy = sy - y0;

        if (x0 < 0 || y0 < 0 || x0 + 1 >= W || y0 + 1 >= H)
          {
            dst[y][x] = 0;
            mask[y][x] = 0;
            continue;
          }
        dst[y][x] = (float) ((src[y0][x0] * (1 - fx) + src[y0][x0 + 1] * fx) * (1 - fy) +
                             (src[y0 + 1][x0] * (1 - fx) + src[y0 + 1][x0 + 1] * fx) * fy);
        mask[y][x] = 1;
      }
}

static double
ncc_at (const float a[H][W], const unsigned char m[H][W],
        const float b[H][W], int dx, int dy, int *count)
{
  double sum = 0;
  int x, y, n = 0;

  for (y = 0; y < H; y++)
    {
      int by = y + dy;

      if (by < 0 || by >= H)
        continue;
      for (x = 0; x < W; x++)
        {
          int bx = x + dx;

          if (bx < 0 || bx >= W || !m[y][x])
            continue;
          sum += a[y][x] * b[by][bx];
          n++;
        }
    }
  *count = n;
  return n > 0 ? sum / n : 0;
}

/* Coarse to fine: 4 px steps over +-8, then 1 px around the winner, and a
 * handful of rotations. A full search would be 10x the work for the same answer
 * at this scale. */
static int
match (const Sample *a, const Sample *b)
{
  static float rot[H][W];
  static unsigned char mask[H][W];
  double best = -2;
  int deg;

  for (deg = -12; deg <= 12; deg += 3)
    {
      int dx, dy, count, bdx = 0, bdy = 0;
      double coarse = -2;

      rotate (a->px, rot, mask, deg);
      for (dy = -8; dy <= 8; dy += 4)
        for (dx = -8; dx <= 8; dx += 4)
          {
            double v = ncc_at (rot, mask, b->px, dx, dy, &count);

            if (count < W * H / 2)
              continue;
            if (v > coarse)
              {
                coarse = v;
                bdx = dx;
                bdy = dy;
              }
          }
      for (dy = bdy - 3; dy <= bdy + 3; dy++)
        for (dx = bdx - 3; dx <= bdx + 3; dx++)
          {
            double v = ncc_at (rot, mask, b->px, dx, dy, &count);

            if (count < W * H / 2)
              continue;
            if (v > best)
              best = v;
          }
    }
  return (int) lround (best * 1000);
}

int
main (int argc, char **argv)
{
  static int score[MAX_FILES][MAX_FILES];
  int i, j;

  setvbuf (stdout, NULL, _IOLBF, 0);
  if (argc < 3)
    {
      fprintf (stderr, "usage: %s <a.pgm> <b.pgm> [...]\n", argv[0]);
      return 2;
    }

  for (i = 1; i < argc && nsamples < MAX_FILES; i++)
    {
      static unsigned char raw[H * W];

      if (strcmp (argv[i], "--hp") == 0)
        {
          use_highpass = 1;
          continue;
        }
      int w = 0, h = 0;

      if (read_pgm (argv[i], &w, &h, raw) < 0 || w != W || h != H)
        {
          fprintf (stderr, "%s: expected %dx%d P5\n", argv[i], W, H);
          continue;
        }
      snprintf (samples[nsamples].name, sizeof (samples[nsamples].name), "%s",
                argv[i]);
      group_of (argv[i], samples[nsamples].group);
      for (j = 0; j < W * H; j++)
        ((float *) samples[nsamples].px)[j] = raw[j];
      if (use_highpass)
        highpass (samples[nsamples].px);
      normalise (samples[nsamples].px);
      nsamples++;
    }

  if (nsamples < 2)
    {
      fprintf (stderr, "need at least two readable captures\n");
      return 1;
    }

  for (i = 0; i < nsamples; i++)
    for (j = i + 1; j < nsamples; j++)
      {
        score[i][j] = match (&samples[i], &samples[j]);
        score[j][i] = score[i][j];
      }

  printf ("\n%-24s %-10s %s\n", "file", "finger", "group");
  for (i = 0; i < nsamples; i++)
    printf ("%-24s %-10s\n", samples[i].name, samples[i].group);

  printf ("\nbest correlation per capture (own group first)\n");
  printf ("%-24s %-10s %s\n", "file", "own", "cross");
  for (i = 0; i < nsamples; i++)
    {
      int own = -1, cross = -1;

      for (j = 0; j < nsamples; j++)
        {
          if (j == i)
            continue;
          if (strcmp (samples[i].group, samples[j].group) == 0)
            {
              if (score[i][j] > own)
                own = score[i][j];
            }
          else if (score[i][j] > cross)
            cross = score[i][j];
        }
      printf ("%-24s %-10d %d\n", samples[i].name, own, cross);
    }

  {
    int floor_genuine = 1 << 20, ceil_impostor = -(1 << 20);

    for (i = 0; i < nsamples; i++)
      for (j = i + 1; j < nsamples; j++)
        {
          if (strcmp (samples[i].group, samples[j].group) == 0)
            {
              if (score[i][j] < floor_genuine)
                floor_genuine = score[i][j];
            }
          else if (score[i][j] > ceil_impostor)
            ceil_impostor = score[i][j];
        }
    printf ("\nworst genuine pair %d, best impostor pair %d\n", floor_genuine,
            ceil_impostor);
    if (ceil_impostor >= floor_genuine)
      printf ("still overlapping\n");
    else
      printf ("separable: threshold %d..%d\n", ceil_impostor + 1,
              floor_genuine);
  }
  return 0;
}
