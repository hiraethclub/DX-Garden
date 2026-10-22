/* Ordered list of stream URLs: favourites.txt and recent.txt. One URL per
 * line, '#' comments allowed. Stations are matched by URL so the lists
 * survive edits to stations.txt. */
#ifndef DXG_USERLIST_H
#define DXG_USERLIST_H

#include <stddef.h>

typedef struct {
    char **urls;
    int count, cap;
} userlist;

#define RECENT_MAX 50

void userlist_init(userlist *l);
void userlist_free(userlist *l);
/* Replaces the contents. Blank lines, comments and duplicates are skipped. */
void userlist_parse(userlist *l, const char *buf, size_t len);
int userlist_index(const userlist *l, const char *url);
/* Adds or removes; returns 1 if the URL is now in the list. */
int userlist_toggle(userlist *l, const char *url);
/* Moves or inserts the URL at the front and trims to `max` entries. */
void userlist_push_front(userlist *l, const char *url, int max);
/* File contents: `header` (comment lines, may be NULL) then one URL per
 * line. Returns a new string. */
char *userlist_serialize(const userlist *l, const char *header);

extern const char FAVOURITES_HEADER[];
extern const char RECENT_HEADER[];

#endif
