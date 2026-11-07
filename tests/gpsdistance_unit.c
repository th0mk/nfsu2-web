/* cc -Wall -Wextra -Werror tests/gpsdistance_unit.c -o out/gpsdistance_unit */
#include "../runtime/gpsdistance_format.h"
#include <assert.h>
#include <string.h>

static void check(float m, int metric, const char *expected){
    char text[32];
    gpsdistance_format(text, sizeof text, m, metric);
    assert(strcmp(text, expected) == 0);
}
int main(void){
    check(0, 1, "0 m");
    check(123.4f, 1, "123 m");
    check(999.4f, 1, "999 m");
    check(999.6f, 1, "1.0 km");
    check(1500, 1, "1.5 km");
    check(0, 0, "0 ft");
    check(30.48f, 0, "100 ft");
    check(1609, 0, "5279 ft");
    check(1609.3f, 0, "1.0 mi");
    check(2414.016f, 0, "1.5 mi");
    return 0;
}
