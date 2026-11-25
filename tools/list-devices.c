/* List the fingerprint devices libfprint can see, without touching them.
 *
 * Discovery only needs udev, so this runs unprivileged: it answers "did the
 * goodix51a0 id table match a spidev node" before anything is installed into
 * /usr/local and before fprintd is restarted.
 *
 *   gcc -o build/list-devices tools/list-devices.c \
 *       $(PKG_CONFIG_PATH=build/stage/usr/local/lib/x86_64-linux-gnu/pkgconfig \
 *         pkg-config --cflags --libs libfprint-2)
 *   LD_LIBRARY_PATH=build/stage/usr/local/lib/x86_64-linux-gnu ./build/list-devices
 */
#include <fprint.h>
#include <stdio.h>

int main (void)
{
    g_autoptr(FpContext) ctx = fp_context_new ();
    GPtrArray *devs;

    fp_context_enumerate (ctx);
    devs = fp_context_get_devices (ctx);

    printf ("devices: %u\n", devs->len);
    for (guint i = 0; i < devs->len; i++)
    {
        FpDevice *dev = g_ptr_array_index (devs, i);
        g_autofree char *name = NULL;
        g_autofree char *id = NULL;
        g_autofree char *drv = NULL;

        g_object_get (dev,
                      "name", &name,
                      "device-id", &id,
                      "driver", &drv,
                      NULL);
        printf ("  [%u] %s\n      id: %s\n      driver: %s\n",
                i, name, id, drv);
    }

    return devs->len == 0;
}
