// Helpers shared by every printer driver (printer_driver.h).
#include "printer_driver.h"

#include <stdio.h>
#include <stdlib.h>

int printer_driver_paper_for(const printer_driver_t *d, int height_px, int dpi)
{
    // 1/100 mm; e.g. a Letter page (279.4 mm) is closer to Letter than to A4 (297 mm)
    long h = (long)height_px * 2540 / dpi;
    int best = 0;
    for (int i = 1; i < d->npapers; i++)
        if (labs(d->papers[i].height - h) < labs(d->papers[best].height - h))
            best = i;
    return best;
}

void printer_driver_urf_copies(const printer_driver_t *d, int copies, char *buf, size_t n)
{
    // V1.4, copies, 8-bit gray (+ sRGB), the resolutions, plain paper, 3 qualities
    int lo = d->resolutions[0], hi = d->resolutions[d->nresolutions - 1];
    char rs[24];
    if (lo == hi)
        snprintf(rs, sizeof(rs), "%d", lo);
    else
        snprintf(rs, sizeof(rs), "%d-%d", lo, hi);
    if (copies < 1)
        copies = 1;
    if (copies > 99)
        copies = 99;
    snprintf(buf, n, "V1.4,CP%d,W8%s,RS%s,MT1,PQ3-4-5", copies, d->color ? ",SRGB24" : "", rs);
}

void printer_driver_urf(const printer_driver_t *d, char *buf, size_t n)
{
    printer_driver_urf_copies(d, 1, buf, n);
}

bool printer_driver_has_dpi(const printer_driver_t *d, int dpi)
{
    for (int i = 0; i < d->nresolutions; i++)
        if (d->resolutions[i] == dpi)
            return true;
    return false;
}
