/* Visualise what NBIS makes of a stored capture.
 *
 * Prints minutia coordinates and writes two PGMs: the input with every detected
 * point marked, and MINDTCT's own binarised ridge map. Judging the marked image
 * tells you whether the detected points sit on real ridge endings (good) or on
 * breaks caused by noise (then the pipeline, not the matcher, is the problem).
 *
 *   ./tools/build-dump-minutiae.sh && ./build/dump-minutiae capture.pgm -o /tmp/x
 *   -> /tmp/x-marked.pgm, /tmp/x-binary.pgm
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
    fgetc (f);
    px = g_malloc (*w * *h);
    if (fread (px, 1, (size_t) *w * *h, f) != (size_t) *w * *h) {
        g_free (px);
        px = NULL;
    }
    fclose (f);
    return px;
}

static void
write_pgm (const char *path, const guchar *px, int w, int h)
{
    FILE *f = fopen (path, "wb");

    if (!f) {
        fprintf (stderr, "cannot write %s\n", path);
        return;
    }
    fprintf (f, "P5\n%d %d\n255\n", w, h);
    fwrite (px, 1, (size_t) w * h, f);
    fclose (f);
}

typedef struct
{
    FpImage   *img;
    GMainLoop *loop;
} Job;

static void
on_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    Job *job = user_data;
    GError *error = NULL;
    GPtrArray *m;

    fp_image_detect_minutiae_finish (FP_IMAGE (src), res, &error);
    if (error) {
        printf ("  error: %s\n", error->message);
        g_clear_error (&error);
        g_main_loop_quit (job->loop);
        return;
    }

    m = fp_image_get_minutiae (FP_IMAGE (src));
    printf ("  minutiae: %d\n", m ? (int) m->len : 0);
    if (m) {
        for (guint i = 0; i < m->len; i++) {
            gint x = -1, y = -1;

            fp_minutia_get_coords (g_ptr_array_index (m, i), &x, &y);
            printf ("    %2u  x=%3d y=%3d\n", i, x, y);
        }
    }
    g_main_loop_quit (job->loop);
}

int
main (int argc, char **argv)
{
    const char *in = NULL, *prefix = "/tmp/minutiae";
    int w = 0, h = 0;
    guchar *px;
    g_autoptr(FpImage) img = NULL;
    gsize len = 0;
    Job job;
    guchar *marked;
    const guchar *bin;
    gsize binlen = 0;
    char path[512];

    setvbuf (stdout, NULL, _IOLBF, 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "-o") == 0 && i + 1 < argc)
            prefix = argv[++i];
        else if (!in)
            in = argv[i];
    }
    if (!in) {
        fprintf (stderr, "usage: %s file.pgm [-o prefix]\n", argv[0]);
        return 2;
    }

    px = read_pgm (in, &w, &h);
    if (!px) {
        fprintf (stderr, "%s: cannot read\n", in);
        return 1;
    }

    img = fp_image_new (w, h);
    marked = g_memdup2 (px, (gsize) w * h);
    memcpy ((guchar *) fp_image_get_data (img, &len), px, len);

    job.img = img;
    job.loop = g_main_loop_new (NULL, FALSE);
    fp_image_detect_minutiae (img, NULL, on_done, &job);
    g_main_loop_run (job.loop);

    /* Re-read the points to paint them. */
    {
        GPtrArray *m = fp_image_get_minutiae (img);

        if (m) {
            for (guint i = 0; i < m->len; i++) {
                gint x = -1, y = -1;

                fp_minutia_get_coords (g_ptr_array_index (m, i), &x, &y);
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int xx = x + dx, yy = y + dy;

                        if (xx < 0 || yy < 0 || xx >= w || yy >= h)
                            continue;
                        /* Hollow marker so the ridge underneath stays visible. */
                        if (abs (dx) == 2 || abs (dy) == 2)
                            marked[yy * w + xx] = 255;
                        else
                            marked[yy * w + xx] = 0;
                    }
            }
        }
    }

    snprintf (path, sizeof (path), "%s-marked.pgm", prefix);
    write_pgm (path, marked, w, h);
    printf ("wrote %s\n", path);

    bin = fp_image_get_binarized (img, &binlen);
    if (bin && binlen == (gsize) w * h) {
        snprintf (path, sizeof (path), "%s-binary.pgm", prefix);
        write_pgm (path, bin, w, h);
        printf ("wrote %s\n", path);
    }

    g_free (marked);
    g_free (px);
    return 0;
}
