/* Visual order of text runs from their bidi embedding levels (Unicode
 * Bidirectional Algorithm, rule L2), for runs already split where the
 * level changes. Even levels are left-to-right, odd right-to-left. */
#ifndef DXG_BIDI_ORDER_H
#define DXG_BIDI_ORDER_H

/* Writes into order[] the run indices from left to right on screen. */
void bidi_visual_order(const unsigned char *levels, int n, int *order);

#endif
