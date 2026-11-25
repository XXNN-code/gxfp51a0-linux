/* Run NBIS minutiae detection on a stored PGM, offline.
 *
 * The sensor is 80x64 which is far smaller than the 500 dpi 256x360 images
 * MINDTCT was tuned on, so it is worth checking parameter variants against a
 * real capture before touching the driver.
 *
 *   gcc -o build/test-minutiae tools/test-minutiae.c \
 *       -Ibuild/stage/usr/local/include/libfprint-2 $(pkg-config --cflags gio-2.0) \
 *       -Lbuild/stage/usr/local/lib/x86_64-linux-gnu -lfprint-2 \
 *       $(pkg-config --libs gio-2.0)
 *   LD_LIBRARY_PATH=build/stage/usr/local/lib/x86_64-linux-gnu \
 *       ./build/test-minutiae finger.pgm
 */
#include <fprint.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        strcmp (magic, "P5") != 0) {
        fclose (f);
        return NULL;
    }
    fgetc (f); /* single whitespace after the header */
    px = g_malloc (*w * *h);
    if (fread (px, 1, (size_t) *w * *h, f) != (size_t) *w * *h) {
        g_free (px);
        px = NULL;
    }
    fclose (f);
    return px;
}

/* Nearest-neighbour upscale, the cheapest thing that makes the ridge period
 * land in MINDTCT's expected range. */
static guchar *
upscale (const guchar *src, int w, int h, int factor, int *ow, int *oh)
{
    guchar *dst;

    *ow = w * factor;
    *oh = h * factor;
    dst = g_malloc (*ow * *oh);
    for (int y = 0; y < *oh; y++)
        for (int x = 0; x < *ow; x++)
            dst[y * *ow + x] = src[(y / factor) * w + (x / factor)];
    return dst;
}

static int detected;

static void
on_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;

    fp_image_detect_minutiae_finish (FP_IMAGE (src), res, &error);
    if (error) {
        printf ("  error: %s\n", error->message);
        g_clear_error (&error);
    } else {
        GPtrArray *m = fp_image_get_minutiae (FP_IMAGE (src));

        detected = m ? (int) m->len : 0;
        printf ("  minutiae: %d\n", detected);
    }
    g_main_loop_quit (user_data);
}

static int
run (const char *label, const guchar *px, int w, int h)
{
    g_autoptr(FpImage) img = fp_image_new (w, h);
    gsize len = 0;
    guchar *dst = (guchar *) fp_image_get_data (img, &len);
    GMainLoop *loop;

    if (len != (gsize) w * h) {
        printf ("%s: unexpected image buffer %zu\n", label, len);
        return -1;
    }
    memcpy (dst, px, len);

    printf ("%s (%dx%d)\n", label, w, h);
    detected = -1;
    loop = g_main_loop_new (NULL, FALSE);
    fp_image_detect_minutiae (img, NULL, on_done, loop);
    g_main_loop_run (loop);
    g_main_loop_unref (loop);
    return detected;
}

int
main (int argc, char **argv)
{
    int w = 0, h = 0, ow = 0, oh = 0;
    guchar *px;

    setvbuf (stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf (stderr, "usage: %s <file.pgm> [file.pgm...]\n", argv[0]);
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        px = read_pgm (argv[i], &w, &h);
        if (!px) {
            fprintf (stderr, "%s: cannot read\n", argv[i]);
            continue;
        }
        printf ("== %s ==\n", argv[i]);
        run ("as-is", px, w, h);
        for (int f = 2; f <= 4; f++) {
            guchar *up = upscale (px, w, h, f, &ow, &oh);
            char label[32];

            snprintf (label, sizeof (label), "x%d", f);
            run (label, up, ow, oh);
            g_free (up);
        }
        g_free (px);
    }
    return 0;
}
