/* Tests for core/bidi_order.c. */
#include <string.h>

#include "bidi_order.h"
#include "test.h"

static int same(const int *a, const int *b, int n)
{
    return memcmp(a, b, (size_t)n * sizeof(int)) == 0;
}

void run_bidi_tests(void)
{
    int order[8];

    {   /* All LTR: unchanged. */
        unsigned char lv[] = { 0, 0, 0 };
        int want[] = { 0, 1, 2 };
        bidi_visual_order(lv, 3, order);
        CHECK(same(order, want, 3));
    }
    {   /* "Radio <arabic> FM" in an LTR paragraph: the Arabic run stays in
         * the middle (its glyphs are reversed by the shaper). */
        unsigned char lv[] = { 0, 1, 0 };
        int want[] = { 0, 1, 2 };
        bidi_visual_order(lv, 3, order);
        CHECK(same(order, want, 3));
    }
    {   /* RTL paragraph: <arabic A> " FM " <arabic B> -> B, FM, A. */
        unsigned char lv[] = { 1, 2, 1 };
        int want[] = { 2, 1, 0 };
        bidi_visual_order(lv, 3, order);
        CHECK(same(order, want, 3));
    }
    {   /* RTL paragraph with digits (level 2) and Latin (level 2):
         * <ar> <digits 2> <ar> <latin 2> <ar> -> reversed run order. */
        unsigned char lv[] = { 1, 2, 1, 2, 1 };
        int want[] = { 4, 3, 2, 1, 0 };
        bidi_visual_order(lv, 5, order);
        CHECK(same(order, want, 5));
    }
    {   /* LTR paragraph with two adjacent RTL runs (different fonts):
         * they swap with each other, as one RTL sequence. */
        unsigned char lv[] = { 0, 1, 1, 0 };
        int want[] = { 0, 2, 1, 3 };
        bidi_visual_order(lv, 4, order);
        CHECK(same(order, want, 4));
    }
    {   /* Nested: LTR text inside RTL inside LTR. */
        unsigned char lv[] = { 0, 1, 2, 1, 0 };
        int want[] = { 0, 3, 2, 1, 4 };
        bidi_visual_order(lv, 5, order);
        CHECK(same(order, want, 5));
    }
    {   /* Single run and empty input. */
        unsigned char lv[] = { 1 };
        bidi_visual_order(lv, 1, order);
        CHECK(order[0] == 0);
        bidi_visual_order(lv, 0, order);
    }
}
