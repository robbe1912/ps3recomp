/* Harness for libs/spurs/spurs_lane_count.h.
 * Usage: <exe> <setting|-> <requested>
 * "-" stands for no setting. Output: the effective lane count. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spurs_lane_count.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    const char *setting = strcmp(argv[1], "-") == 0 ? NULL : argv[1];
    printf("%u\n", spurs_lane_count((uint32_t)strtoul(argv[2], NULL, 10), setting));
    return 0;
}
