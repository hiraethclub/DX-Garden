#include "bidi_order.h"

static void reverse(int *a, int i, int j)
{
    while (i < j) {
        int t = a[i];
        a[i++] = a[j];
        a[j--] = t;
    }
}

void bidi_visual_order(const unsigned char *levels, int n, int *order)
{
    int i, max = 0, min_odd = 255, lvl;

    for (i = 0; i < n; i++) {
        order[i] = i;
        if (levels[i] > max)
            max = levels[i];
        if ((levels[i] & 1) && levels[i] < min_odd)
            min_odd = levels[i];
    }
    if (min_odd == 255)
        return; /* all left-to-right */
    /* L2: from the highest level down to the lowest odd level, reverse
     * every maximal sequence of runs at that level or higher. */
    for (lvl = max; lvl >= min_odd; lvl--) {
        i = 0;
        while (i < n) {
            int j;
            if (levels[order[i]] < lvl) {
                i++;
                continue;
            }
            j = i;
            while (j + 1 < n && levels[order[j + 1]] >= lvl)
                j++;
            reverse(order, i, j);
            i = j + 1;
        }
    }
}
