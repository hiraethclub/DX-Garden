/*
 * ICY (Shoutcast/Icecast) in-stream metadata.
 *
 * When a client sends "Icy-MetaData: 1" and the server answers with
 * "icy-metaint: N", the body is N audio bytes, then one length byte L,
 * then L*16 bytes of metadata (StreamTitle='...';StreamUrl='...';), then
 * N audio bytes again, and so on. The demuxer strips the metadata out and
 * reports title changes.
 */
#ifndef DXG_ICY_H
#define DXG_ICY_H

#include <stddef.h>

#define ICY_TITLE_MAX 256

typedef struct {
    size_t metaint;    /* 0: no metadata in this stream */
    size_t audio_left; /* audio bytes before the next length byte */
    size_t meta_len;   /* bytes in the current metadata block */
    size_t meta_got;
    int in_meta;       /* 1 while reading a metadata block */
    int want_len;      /* 1 when the next byte is a length byte */
    char meta[16 * 255 + 1];
    char title[ICY_TITLE_MAX]; /* UTF-8, "" if none */
    int title_changed;         /* set when `title` changes; caller clears */
} icy_demux;

/* metaint 0 passes everything through as audio. */
void icy_init(icy_demux *d, size_t metaint);

/* Splits n input bytes: the audio goes to `out` (which may be `in`; it
 * needs room for n bytes), metadata is consumed. Returns the number of
 * audio bytes written. */
size_t icy_feed(icy_demux *d, const unsigned char *in, size_t n, unsigned char *out);

/* Extracts StreamTitle from a metadata block into title (UTF-8; Latin-1
 * text is converted). Returns 1 if a StreamTitle was present. */
int icy_parse_title(const char *meta, size_t len, char *title, size_t n);

#endif
