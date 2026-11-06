#ifndef NFSU2_GPSDISTANCE_FORMAT_H
#define NFSU2_GPSDISTANCE_FORMAT_H
#include <stdio.h>

/* Match Speedometer Units. Switch before rounding would display 1000 m or
 * 5280 ft; long distances use one decimal place. Input is world metres. */
static inline void gpsdistance_format(char *text, size_t size, float metres, int metric){
    double small = metric ? metres : metres / 0.3048;
    double threshold = metric ? 1000.0 : 5280.0;
    if (small < threshold - 0.5)
        snprintf(text, size, "%.0f %s", small, metric ? "m" : "ft");
    else
        snprintf(text, size, "%.1f %s", small / threshold, metric ? "km" : "mi");
}
#endif
