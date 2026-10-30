#include "player.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <mpg123.h>

#include "icy.h"
#include "log.h"
#include "stream_buffer.h"
#include "stream_info.h"

#define GRAIN 1024        /* samples per push to the audio stream */
#define READ_CHUNK 4096   /* compressed bytes per decoder read */
#define USER_AGENT "DXGarden/" DXG_VERSION " (" DXG_PLATFORM_NAME ")"
/* Decoded audio queued ahead of the device. SDL pulls from this on its own
 * thread; more than this and the decoder waits, which keeps the stream
 * buffer (not the device queue) holding the slack. */
#define QUEUE_MS 250
#define PLAYLIST_MAX (64 * 1024)   /* largest playlist body read */
#define PLAYLIST_DEPTH 3           /* playlists pointing at playlists */
#define STABLE_BYTES (256 * 1024)  /* a connection this long resets the retry count */
/* A stream slower than this for STALL_S is treated as dropped. A full ring
 * still reads at the stream's own bitrate, and the slowest MP3 streams
 * (16 kbps) send 2 KB/s, so only a stall trickles below it. 00.21 waited
 * for 20 s of nothing, and a trickling server gave 45 s of silence. */
#define STALL_BYTES_PER_S 1000L
#define STALL_S 10L

typedef struct {
    SDL_Mutex *lock;
    SDL_Condition *data_cond;  /* decoder waits for data */
    SDL_Condition *space_cond; /* network waits for space */
    stream_buffer sb;

    /* Session. Everything below is guarded by `lock`. */
    int active;
    int stop;      /* threads should wind down */
    int user_stop; /* the stop was asked for, so it is not an error */
    int net_done, dec_done;
    SDL_Thread *net_thid, *dec_thid;
    char url[512];
    char pending_url[512];
    size_t pending_prebuffer;

    CURL *curl;
    int connected;     /* this connection has sent a reply */
    long http_status;
    char content_type[64];
    char icy_name[96];
    char cur_url[512]; /* what is being fetched (a playlist's target, say) */
    char title[ICY_TITLE_MAX]; /* now playing, from ICY metadata */
    int reconnecting;  /* attempt number while reconnecting, else 0 */
    uint64_t retry_at; /* SDL_GetTicksNS() */
    char reconnect_why[128];

    /* Network-thread only, per connection. */
    size_t metaint;
    int mode;          /* MODE_* below, decided by the first data */
    char fatal[256];   /* set by callbacks: don't retry, show this */
    char *plbuf;
    size_t pl_len;
    uint64_t conn_bytes;

    long rate;
    int channels;
    int bitrate_kbps;
    int mpeg_version;
    int layer;
    uint64_t decode_us;
    uint64_t samples_out;

    char error[256];
} player;

static player P;

static void lock(void)
{
    SDL_LockMutex(P.lock);
}

static void unlock(void)
{
    SDL_UnlockMutex(P.lock);
}

/* Record the first error of a session; later ones are usually fallout.
 * Call with the lock held. */
static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_error(const char *fmt, ...)
{
    va_list ap;

    if (P.error[0])
        return;
    va_start(ap, fmt);
    vsnprintf(P.error, sizeof(P.error), fmt, ap);
    va_end(ap);
    LOG("error: %s", P.error);
}

/* ---- network thread ---------------------------------------------------- */

enum { MODE_UNDECIDED, MODE_AUDIO, MODE_PLAYLIST, MODE_REFUSED };

static icy_demux icy;                       /* network thread only */
static unsigned char demux_out[16 * 1024];

/* Copies a header value (after "name:") without spaces or line ends. */
static void header_value(const char *v, size_t n, char *out, size_t outn)
{
    while (n && (*v == ' ' || *v == '\t')) {
        v++;
        n--;
    }
    while (n && (v[n - 1] == '\r' || v[n - 1] == '\n' || v[n - 1] == ' '))
        n--;
    if (n >= outn)
        n = outn - 1;
    memcpy(out, v, n);
    out[n] = '\0';
}

static size_t on_header(char *buf, size_t size, size_t nmemb, void *ud)
{
    size_t len = size * nmemb;
    char val[96];
    (void)ud;

    /* Each response (redirects included) starts with a status line. */
    if (len > 5 && (strncasecmp(buf, "HTTP/", 5) == 0 || strncasecmp(buf, "ICY ", 4) == 0)) {
        P.metaint = 0;
    } else if (len > 9 && strncasecmp(buf, "icy-name:", 9) == 0) {
        header_value(buf + 9, len - 9, val, sizeof(val));
        lock();
        snprintf(P.icy_name, sizeof(P.icy_name), "%s", val);
        unlock();
    } else if (len > 12 && strncasecmp(buf, "icy-metaint:", 12) == 0) {
        long m;
        header_value(buf + 12, len - 12, val, sizeof(val));
        m = strtol(val, NULL, 10);
        P.metaint = m > 0 && m < 1024 * 1024 ? (size_t)m : 0;
    }
    return len;
}

/* First data of a connection: log it and decide what it is. Lock held. */
static void first_data(const unsigned char *buf, size_t n)
{
    char *ct = NULL, *eff = NULL, format[16];
    curl_off_t dns = 0, tcp = 0, tls = 0, first = 0;
    long redirects = 0;
    stream_kind kind;

    P.connected = 1;
    curl_easy_getinfo(P.curl, CURLINFO_RESPONSE_CODE, &P.http_status);
    curl_easy_getinfo(P.curl, CURLINFO_CONTENT_TYPE, &ct);
    snprintf(P.content_type, sizeof(P.content_type), "%s", ct ? ct : "");
    LOG("connected: HTTP %ld, type '%s', name '%s', metadata every %u bytes", P.http_status,
        P.content_type, P.icy_name, (unsigned)P.metaint);
    /* Where the time to first byte went, in ms. */
    curl_easy_getinfo(P.curl, CURLINFO_NAMELOOKUP_TIME_T, &dns);
    curl_easy_getinfo(P.curl, CURLINFO_CONNECT_TIME_T, &tcp);
    curl_easy_getinfo(P.curl, CURLINFO_APPCONNECT_TIME_T, &tls);
    curl_easy_getinfo(P.curl, CURLINFO_STARTTRANSFER_TIME_T, &first);
    curl_easy_getinfo(P.curl, CURLINFO_REDIRECT_COUNT, &redirects);
    curl_easy_getinfo(P.curl, CURLINFO_EFFECTIVE_URL, &eff);
    LOG("timing: dns %lld ms, tcp %lld ms, tls %lld ms, first byte %lld ms, redirects %ld",
        (long long)dns / 1000, (long long)tcp / 1000, (long long)tls / 1000,
        (long long)first / 1000, redirects);
    if (redirects && eff)
        LOG("redirected to %s", eff);

    kind = stream_classify(P.content_type, eff ? eff : P.cur_url, buf, n, format, sizeof(format));
    switch (kind) {
    case SK_MP3:
    case SK_UNKNOWN:
        P.mode = MODE_AUDIO;
        icy_init(&icy, P.metaint);
        P.reconnecting = 0;
        break;
    case SK_PLAYLIST:
        P.mode = MODE_PLAYLIST;
        LOG("net: the address is a playlist, reading it");
        break;
    case SK_HLS:
        P.mode = MODE_REFUSED;
        snprintf(P.fatal, sizeof(P.fatal),
                 "This station uses HLS (segmented streaming), which DX Garden doesn't play");
        break;
    case SK_OTHER:
        P.mode = MODE_REFUSED;
        snprintf(P.fatal, sizeof(P.fatal),
                 "This station streams %s; DX Garden only plays MP3", format);
        break;
    case SK_HTML:
        P.mode = MODE_REFUSED;
        snprintf(P.fatal, sizeof(P.fatal), "That address is a web page, not a stream");
        break;
    }
}

/* Audio into the stream buffer, waiting for space. Lock held; returns 0 if
 * stopped. */
static int push_audio(const unsigned char *a, size_t n)
{
    size_t done = 0;
    while (done < n && !P.stop) {
        size_t k = sb_write(&P.sb, a + done, n - done);
        done += k;
        if (k)
            SDL_SignalCondition(P.data_cond);
        if (done < n)
            SDL_WaitCondition(P.space_cond, P.lock);
    }
    return !P.stop;
}

static size_t on_data(char *buf, size_t size, size_t nmemb, void *ud)
{
    size_t total = size * nmemb, off = 0;
    int ok = 1;
    (void)ud;

    lock();
    if (P.mode == MODE_UNDECIDED)
        first_data((const unsigned char *)buf, total);
    P.conn_bytes += total;

    if (P.mode == MODE_PLAYLIST) {
        if (P.pl_len + total > PLAYLIST_MAX) {
            snprintf(P.fatal, sizeof(P.fatal), "The playlist is too large to read");
            ok = 0;
        } else {
            char *nb = realloc(P.plbuf, P.pl_len + total + 1);
            if (!nb) {
                ok = 0;
            } else {
                P.plbuf = nb;
                memcpy(P.plbuf + P.pl_len, buf, total);
                P.pl_len += total;
            }
        }
    } else if (P.mode == MODE_REFUSED) {
        ok = 0;
    } else {
        while (ok && off < total) {
            size_t chunk = total - off < sizeof(demux_out) ? total - off : sizeof(demux_out);
            size_t a = icy_feed(&icy, (const unsigned char *)buf + off, chunk, demux_out);
            off += chunk;
            if (icy.title_changed) {
                icy.title_changed = 0;
                memcpy(P.title, icy.title, sizeof(P.title));
                LOG("title: %s", P.title[0] ? P.title : "(none)");
            }
            ok = push_audio(demux_out, a);
        }
        if (P.stop)
            ok = 0;
    }
    unlock();
    /* Returning less than `total` makes curl abort the transfer. */
    return ok ? total : 0;
}

static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut,
                       curl_off_t un)
{
    int stopped;
    (void)ud; (void)dt; (void)dn; (void)ut; (void)un;

    lock();
    stopped = P.stop;
    unlock();
    return stopped;
}

typedef enum { OUT_STOPPED, OUT_PLAYLIST, OUT_DROPPED, OUT_FATAL } outcome;

/* One request for P.cur_url. Puts a message in `msg` for DROPPED/FATAL and
 * the next URL in `next` for PLAYLIST. */
static outcome fetch_once(CURL *c, char *msg, size_t mn, char *next, size_t nn)
{
    char errbuf[CURL_ERROR_SIZE] = "";
    struct curl_slist *hdrs = curl_slist_append(NULL, "Icy-MetaData: 1");
    CURLcode rc;
    long code = 0;
    outcome out;

    lock();
    P.connected = 0;
    P.mode = MODE_UNDECIDED;
    P.fatal[0] = '\0';
    P.metaint = 0;
    P.conn_bytes = 0;
    free(P.plbuf);
    P.plbuf = NULL;
    P.pl_len = 0;
    unlock();

    curl_easy_setopt(c, CURLOPT_URL, P.cur_url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
#ifdef _WIN32
    /* Schannel checks against the Windows certificate store; revocation
     * checks fail behind some proxies and on stations with private CAs. */
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NO_REVOKE);
#endif
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, STALL_BYTES_PER_S);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, STALL_S);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);

    LOG("net: GET %s", P.cur_url);
    rc = curl_easy_perform(c);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(hdrs);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    LOG("net: finished, curl code %d, %llu bytes", (int)rc, (unsigned long long)P.conn_bytes);

    lock();
    msg[0] = '\0';
    if (P.stop) {
        out = OUT_STOPPED;
    } else if (P.fatal[0]) {
        snprintf(msg, mn, "%s", P.fatal);
        out = OUT_FATAL;
    } else if (P.mode == MODE_PLAYLIST && rc == CURLE_OK) {
        int r = P.plbuf ? (P.plbuf[P.pl_len] = '\0', playlist_first_url(P.plbuf, P.pl_len, next, nn))
                        : 0;
        if (r == 1) {
            out = OUT_PLAYLIST;
        } else {
            snprintf(msg, mn, r < 0 ? "The playlist is an HLS stream, which DX Garden doesn't play"
                                    : "The playlist has no stream address in it");
            out = OUT_FATAL;
        }
    } else if (rc == CURLE_HTTP_RETURNED_ERROR) {
        http_status_message(code, msg, mn);
        out = http_status_retry(code) ? OUT_DROPPED : OUT_FATAL;
    } else {
        out = OUT_DROPPED;
        switch (rc) {
        case CURLE_OK:
            snprintf(msg, mn, "The server ended the stream");
            break;
        case CURLE_COULDNT_RESOLVE_HOST:
            snprintf(msg, mn, "Server not found (DNS): check the network and the address");
            break;
        case CURLE_COULDNT_CONNECT:
            snprintf(msg, mn, "Couldn't connect to the server");
            break;
        case CURLE_OPERATION_TIMEDOUT:
            snprintf(msg, mn, P.connected ? "The stream stalled (under 1 KB/s for 10 s)"
                                          : "No answer from the server");
            break;
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_PARTIAL_FILE:
        case CURLE_GOT_NOTHING:
            snprintf(msg, mn, "The connection dropped");
            break;
        case CURLE_PEER_FAILED_VERIFICATION:
            snprintf(msg, mn, "The server's security certificate isn't valid");
            out = OUT_FATAL;
            break;
        case CURLE_SSL_CACERT_BADFILE:
            snprintf(msg, mn, "Can't check secure servers: no certificate store");
            out = OUT_FATAL;
            break;
        case CURLE_TOO_MANY_REDIRECTS:
            snprintf(msg, mn, "Too many redirects");
            out = OUT_FATAL;
            break;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
            snprintf(msg, mn, "The stream address isn't valid");
            out = OUT_FATAL;
            break;
        default:
            snprintf(msg, mn, "%s%s%s", curl_easy_strerror(rc), errbuf[0] ? ": " : "", errbuf);
            break;
        }
    }
    free(P.plbuf);
    P.plbuf = NULL;
    P.pl_len = 0;
    unlock();
    return out;
}

/* Sleeps up to ms, waking early on stop. Returns 0 if stopped. */
static int wait_or_stop(unsigned ms)
{
    unsigned waited = 0;
    while (waited < ms) {
        int stopped;
        lock();
        stopped = P.stop;
        unlock();
        if (stopped)
            return 0;
        SDL_Delay(100);
        waited += 100;
    }
    return 1;
}

static int SDLCALL net_thread(void *argp)
{
    char msg[256], next[512];
    int attempt = 0, depth = 0;
    CURL *c;
    (void)argp;

    c = curl_easy_init();
    if (!c) {
        lock();
        set_error("Could not start network (curl init failed)");
        sb_set_eof(&P.sb);
        P.net_done = 1;
        SDL_BroadcastCondition(P.data_cond);
        unlock();
        return 0;
    }
    lock();
    P.curl = c;
    snprintf(P.cur_url, sizeof(P.cur_url), "%s", P.url);
    unlock();

    for (;;) {
        outcome o = fetch_once(c, msg, sizeof(msg), next, sizeof(next));

        if (o == OUT_STOPPED)
            break;
        if (o == OUT_PLAYLIST) {
            if (++depth > PLAYLIST_DEPTH) {
                lock();
                set_error("Playlists point to more playlists; giving up");
                unlock();
                break;
            }
            LOG("net: playlist points to %s", next);
            lock();
            snprintf(P.cur_url, sizeof(P.cur_url), "%s", next);
            unlock();
            continue;
        }
        if (o == OUT_FATAL) {
            lock();
            set_error("%s", msg);
            unlock();
            break;
        }
        /* Dropped: try again after a pause, while the buffer plays on. A
         * connection that ran for a while starts the count again. */
        if (P.conn_bytes >= STABLE_BYTES)
            attempt = 0;
        if (++attempt > RECONNECT_MAX_ATTEMPTS) {
            lock();
            set_error("%s (gave up after %d tries)", msg, RECONNECT_MAX_ATTEMPTS);
            unlock();
            break;
        }
        {
            unsigned delay = reconnect_delay_ms(attempt);
            LOG("net: %s; reconnecting in %u s (attempt %d of %d)", msg, delay / 1000, attempt,
                RECONNECT_MAX_ATTEMPTS);
            lock();
            P.reconnecting = attempt;
            P.retry_at = SDL_GetTicksNS() + (uint64_t)delay * 1000000;
            snprintf(P.reconnect_why, sizeof(P.reconnect_why), "%s", msg);
            unlock();
            if (!wait_or_stop(delay))
                break;
        }
        /* Reconnect to the station's own address, which may have been a
         * playlist with a fresh target. */
        depth = 0;
        lock();
        snprintf(P.cur_url, sizeof(P.cur_url), "%s", P.url);
        unlock();
    }

    lock();
    P.curl = NULL;
    P.reconnecting = 0;
    sb_set_eof(&P.sb);
    P.net_done = 1;
    SDL_BroadcastCondition(P.data_cond);
    unlock();

    curl_easy_cleanup(c);
    return 0;
}

/* ---- decode thread ----------------------------------------------------- */

static int16_t pcm[GRAIN * 2];
static unsigned char in_buf[READ_CHUNK];

/* Output volume in percent, set by the UI thread (sleep timer fade). */
static SDL_AtomicInt volume_pct = {100};

/* One stream to the default device for the whole session. SDL converts
 * from the station's rate, so a rate change only changes the input side. */
static SDL_AudioStream *open_output(long rate)
{
    SDL_AudioSpec spec;
    SDL_AudioStream *as;

    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = (int)rate;
    as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!as) {
        lock();
        set_error("Audio output refused %ld Hz: %s", rate, SDL_GetError());
        unlock();
        return NULL;
    }
    SDL_ResumeAudioStreamDevice(as);
    LOG("audio: default device open at %ld Hz", rate);
    return as;
}

/* Hands one grain to SDL, first waiting while QUEUE_MS is already queued,
 * so the decoder runs at the stream's own pace. Returns 0 if stopped. */
static int output_grain(SDL_AudioStream *as, long rate)
{
    int max_bytes = (int)(rate * 4 * QUEUE_MS / 1000);
    for (;;) {
        int stopped;
        lock();
        stopped = P.stop;
        unlock();
        if (stopped)
            return 0;
        if (SDL_GetAudioStreamQueued(as) < max_bytes)
            break;
        SDL_Delay(5);
    }
    return SDL_PutAudioStreamData(as, pcm, sizeof(pcm));
}

static int SDLCALL dec_thread(void *argp)
{
    mpg123_handle *mh;
    const long *rates;
    size_t nrates, i, filled = 0;
    int err, failed = 0, applied_volume = -1;
    long out_rate = 0;
    SDL_AudioStream *as = NULL;
    (void)argp;

    mh = mpg123_new(NULL, &err);
    if (!mh) {
        lock();
        set_error("MP3 decoder init failed: %s", mpg123_plain_strerror(err));
        failed = 1;
        unlock();
        goto out;
    }
    /* Always 16-bit stereo out; mono streams get duplicated channels. */
    mpg123_param(mh, MPG123_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET, 0);
    mpg123_format_none(mh);
    mpg123_rates(&rates, &nrates);
    for (i = 0; i < nrates; i++)
        mpg123_format(mh, rates[i], MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_open_feed(mh);

    for (;;) {
        size_t n;
        uint64_t t0;
        int ret;

        lock();
        while (!P.stop && sb_get_state(&P.sb) == SB_BUFFERING)
            SDL_WaitCondition(P.data_cond, P.lock);
        if (P.stop || sb_get_state(&P.sb) == SB_DRAINED) {
            unlock();
            break;
        }
        n = sb_read(&P.sb, in_buf, sizeof(in_buf));
        SDL_SignalCondition(P.space_cond);
        unlock();

        t0 = SDL_GetTicksNS();
        if (mpg123_feed(mh, in_buf, n) != MPG123_OK) {
            lock();
            set_error("MP3 decoder: %s", mpg123_strerror(mh));
            unlock();
            failed = 1;
            break;
        }

        for (;;) {
            size_t done = 0;
            ret = mpg123_read(mh, (unsigned char *)pcm + filled, sizeof(pcm) - filled, &done);
            filled += done;

            if (ret == MPG123_NEW_FORMAT) {
                long rate;
                int ch, enc;
                mpg123_getformat(mh, &rate, &ch, &enc);
                LOG("decoder: format %ld Hz, %d ch, enc 0x%x", rate, ch, enc);
                if (!as) {
                    as = open_output(rate);
                    if (!as) {
                        failed = 1;
                        break;
                    }
                } else if (rate != out_rate) {
                    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, (int)rate };
                    SDL_SetAudioStreamFormat(as, &spec, NULL);
                }
                out_rate = rate;
                continue;
            }
            if (ret == MPG123_ERR) {
                lock();
                set_error("MP3 decoder: %s", mpg123_strerror(mh));
                unlock();
                failed = 1;
                break;
            }
            if (filled == sizeof(pcm)) {
                int v = SDL_GetAtomicInt(&volume_pct);
                lock();
                P.decode_us += (SDL_GetTicksNS() - t0) / 1000;
                P.samples_out += GRAIN;
                unlock();
                if (v != applied_volume) {
                    applied_volume = v;
                    SDL_SetAudioStreamGain(as, v / 100.0f);
                }
                if (!output_grain(as, out_rate))
                    break;
                t0 = SDL_GetTicksNS();
                filled = 0;
            }
            if (ret == MPG123_NEED_MORE)
                break;
        }
        if (failed)
            break;

        {
            struct mpg123_frameinfo fi;
            uint64_t dt = (SDL_GetTicksNS() - t0) / 1000;
            int have_info = mpg123_info(mh, &fi) == MPG123_OK;
            lock();
            P.decode_us += dt;
            if (have_info) {
                P.rate = fi.rate;
                P.channels = fi.mode == MPG123_M_MONO ? 1 : 2;
                P.bitrate_kbps = fi.bitrate;
                P.mpeg_version = fi.version == MPG123_1_0 ? 1
                               : fi.version == MPG123_2_0 ? 2 : 25;
                P.layer = fi.layer;
            }
            unlock();
        }
    }

out:
    if (as)
        SDL_DestroyAudioStream(as);
    if (mh)
        mpg123_delete(mh);

    lock();
    if (failed) {
        /* Take the network thread down with us. */
        P.stop = 1;
        SDL_BroadcastCondition(P.space_cond);
    }
    LOG("decoder: finished%s", failed ? " with error" : "");
    P.dec_done = 1;
    unlock();
    return 0;
}

/* ---- public API -------------------------------------------------------- */

int player_init(size_t ring_capacity)
{
    int err;

    memset(&P, 0, sizeof(P));
    P.lock = SDL_CreateMutex();
    P.data_cond = SDL_CreateCondition();
    P.space_cond = SDL_CreateCondition();
    if (!P.lock || !P.data_cond || !P.space_cond) {
        LOG("player: could not create locks: %s", SDL_GetError());
        return -1;
    }
    if (sb_init(&P.sb, ring_capacity, ring_capacity / 8) != 0) {
        LOG("player: could not allocate %u byte buffer", (unsigned)ring_capacity);
        return -1;
    }
    err = mpg123_init();
    if (err != MPG123_OK) {
        LOG("player: mpg123_init failed: %s", mpg123_plain_strerror(err));
        return -1;
    }
    LOG("player: ring %u KB, %s", (unsigned)(ring_capacity / 1024),
        curl_version());
    return 0;
}

static void start_session(const char *url, size_t prebuffer)
{
    snprintf(P.url, sizeof(P.url), "%s", url);
    sb_reset(&P.sb);
    sb_set_prebuffer(&P.sb, prebuffer);
    P.stop = P.user_stop = 0;
    P.net_done = P.dec_done = 0;
    P.connected = 0;
    P.http_status = 0;
    P.content_type[0] = P.icy_name[0] = P.error[0] = '\0';
    P.title[0] = P.reconnect_why[0] = '\0';
    P.reconnecting = 0;
    P.rate = 0;
    P.channels = P.bitrate_kbps = P.mpeg_version = P.layer = 0;
    P.decode_us = P.samples_out = 0;

    LOG("player: start %s (prebuffer %u KB)", url, (unsigned)(prebuffer / 1024));
    /* The caller holds the lock, so the threads can't run ahead of the
     * session state set here. */
    P.net_thid = SDL_CreateThread(net_thread, "dxg_net", NULL);
    P.dec_thid = SDL_CreateThread(dec_thread, "dxg_decode", NULL);
    if (!P.net_thid || !P.dec_thid) {
        LOG("player: thread create failed: %s", SDL_GetError());
        snprintf(P.error, sizeof(P.error), "Could not create player threads");
        /* Whichever thread did start sees P.stop and winds down; the
         * missing one counts as done so player_update() can reap. */
        P.stop = 1;
        P.net_done = !P.net_thid;
        P.dec_done = !P.dec_thid;
        P.active = P.net_thid || P.dec_thid;
        return;
    }
    P.active = 1;
}

static void request_stop(void)
{
    P.stop = 1;
    P.user_stop = 1;
    SDL_BroadcastCondition(P.data_cond);
    SDL_BroadcastCondition(P.space_cond);
}

void player_start(const char *url, size_t prebuffer)
{
    lock();
    if (P.active) {
        /* Queue it; player_update() starts it once the old threads exit. */
        LOG("player: switch requested");
        snprintf(P.pending_url, sizeof(P.pending_url), "%s", url);
        P.pending_prebuffer = prebuffer;
        request_stop();
    } else {
        start_session(url, prebuffer);
    }
    unlock();
}

void player_stop(void)
{
    lock();
    P.pending_url[0] = '\0';
    if (P.active) {
        LOG("player: stop requested");
        request_stop();
    }
    unlock();
}

void player_set_prebuffer(size_t prebuffer)
{
    lock();
    sb_set_prebuffer(&P.sb, prebuffer);
    SDL_BroadcastCondition(P.data_cond);
    unlock();
}

void player_set_volume(int pct)
{
    SDL_SetAtomicInt(&volume_pct, pct < 0 ? 0 : pct > 100 ? 100 : pct);
}

void player_update(void)
{
    SDL_Thread *net, *dec;

    lock();
    if (!P.active || !P.net_done || !P.dec_done) {
        unlock();
        return;
    }
    net = P.net_thid;
    dec = P.dec_thid;
    unlock();

    /* Both threads have set their done flag, so these return at once. */
    if (net)
        SDL_WaitThread(net, NULL);
    if (dec)
        SDL_WaitThread(dec, NULL);

    lock();
    P.active = 0;
    if (P.user_stop)
        P.error[0] = '\0';
    LOG("player: session ended");
    if (P.pending_url[0]) {
        char url[512];
        snprintf(url, sizeof(url), "%s", P.pending_url);
        P.pending_url[0] = '\0';
        start_session(url, P.pending_prebuffer);
    }
    unlock();
}

void player_get_status(player_status *s)
{
    memset(s, 0, sizeof(*s));
    lock();
    if (!P.active)
        s->state = P.error[0] ? PLAYER_ERROR : PLAYER_IDLE;
    else if (P.stop)
        s->state = P.user_stop ? PLAYER_STOPPING : PLAYER_ERROR;
    else if (P.reconnecting)
        s->state = PLAYER_RECONNECTING;
    else if (!P.connected)
        s->state = PLAYER_CONNECTING;
    else if (sb_get_state(&P.sb) == SB_BUFFERING)
        s->state = PLAYER_BUFFERING;
    else
        s->state = PLAYER_PLAYING;

    s->prebuffer_pct = sb_prebuffer_percent(&P.sb);
    s->fill_pct = sb_fill_percent(&P.sb);
    s->fill = sb_fill(&P.sb);
    s->capacity = sb_capacity(&P.sb);
    s->prebuffer = P.sb.prebuffer;
    s->underruns = P.sb.underruns;
    s->bytes_in = P.sb.total_in;
    s->http_status = P.http_status;
    memcpy(s->content_type, P.content_type, sizeof(s->content_type));
    memcpy(s->icy_name, P.icy_name, sizeof(s->icy_name));
    s->rate = P.rate;
    s->channels = P.channels;
    s->bitrate_kbps = P.bitrate_kbps;
    s->mpeg_version = P.mpeg_version;
    s->layer = P.layer;
    if (P.samples_out && P.rate)
        s->decode_load_pct = (unsigned)(P.decode_us * P.rate / 10000 / P.samples_out);
    memcpy(s->error, P.error, sizeof(s->error));
    memcpy(s->title, P.title, sizeof(s->title));
    s->reconnect_attempt = P.reconnecting;
    s->reconnect_max = RECONNECT_MAX_ATTEMPTS;
    {
        uint64_t now = SDL_GetTicksNS();
        s->retry_in_s = P.reconnecting && P.retry_at > now
                            ? (unsigned)((P.retry_at - now + 999999999) / 1000000000) : 0;
    }
    memcpy(s->reconnect_why, P.reconnect_why, sizeof(s->reconnect_why));
    unlock();
}

void player_shutdown(void)
{
    int i;

    player_stop();
    /* Give the threads up to 3 s; a stuck DNS lookup can't be interrupted. */
    for (i = 0; i < 300; i++) {
        player_update();
        lock();
        if (!P.active) {
            unlock();
            break;
        }
        unlock();
        SDL_Delay(10);
    }
    mpg123_exit();
}
