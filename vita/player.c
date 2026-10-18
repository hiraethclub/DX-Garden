#include "player.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>
#include <mpg123.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "log.h"
#include "stream_buffer.h"

#define GRAIN 1024        /* samples per sceAudioOutOutput call */
#define READ_CHUNK 4096   /* compressed bytes per decoder read */
#define CA_BUNDLE "app0:cacert.pem"
#define USER_AGENT "DXGarden/" DXG_VERSION " (PS Vita)"

#define THREAD_PRIO_DEFAULT 0x10000100
#define NET_STACK (512 * 1024) /* mbedTLS handshakes are stack hungry */
#define DEC_STACK (256 * 1024)

typedef struct {
    SceKernelLwMutexWork lock;
    SceKernelLwCondWork data_cond;  /* decoder waits for data */
    SceKernelLwCondWork space_cond; /* network waits for space */
    stream_buffer sb;

    /* Session. Everything below is guarded by `lock`. */
    int active;
    int stop;      /* threads should wind down */
    int user_stop; /* the stop was asked for, so it is not an error */
    int net_done, dec_done;
    SceUID net_thid, dec_thid;
    char url[512];
    char pending_url[512];
    size_t pending_prebuffer;

    CURL *curl;
    int connected;
    long http_status;
    char content_type[64];
    char icy_name[96];

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
    sceKernelLockLwMutex(&P.lock, 1, NULL);
}

static void unlock(void)
{
    sceKernelUnlockLwMutex(&P.lock, 1);
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

static size_t on_header(char *buf, size_t size, size_t nmemb, void *ud)
{
    size_t len = size * nmemb;
    static const char key[] = "icy-name:";
    (void)ud;

    if (len > sizeof(key) - 1 && strncasecmp(buf, key, sizeof(key) - 1) == 0) {
        const char *v = buf + sizeof(key) - 1;
        size_t n = len - (sizeof(key) - 1);
        while (n && (*v == ' ' || *v == '\t')) {
            v++;
            n--;
        }
        while (n && (v[n - 1] == '\r' || v[n - 1] == '\n' || v[n - 1] == ' '))
            n--;
        if (n >= sizeof(P.icy_name))
            n = sizeof(P.icy_name) - 1;
        lock();
        memcpy(P.icy_name, v, n);
        P.icy_name[n] = '\0';
        unlock();
    }
    return len;
}

static size_t on_data(char *buf, size_t size, size_t nmemb, void *ud)
{
    size_t total = size * nmemb, done = 0;
    int stopped;
    (void)ud;

    lock();
    if (!P.connected) {
        char *ct = NULL;
        P.connected = 1;
        curl_easy_getinfo(P.curl, CURLINFO_RESPONSE_CODE, &P.http_status);
        curl_easy_getinfo(P.curl, CURLINFO_CONTENT_TYPE, &ct);
        snprintf(P.content_type, sizeof(P.content_type), "%s", ct ? ct : "");
        LOG("connected: HTTP %ld, type '%s', name '%s'", P.http_status,
            P.content_type, P.icy_name);
        {
            /* Where the time to first byte went, in ms. */
            curl_off_t dns = 0, tcp = 0, tls = 0, first = 0;
            long redirects = 0;
            char *eff = NULL;
            curl_easy_getinfo(P.curl, CURLINFO_NAMELOOKUP_TIME_T, &dns);
            curl_easy_getinfo(P.curl, CURLINFO_CONNECT_TIME_T, &tcp);
            curl_easy_getinfo(P.curl, CURLINFO_APPCONNECT_TIME_T, &tls);
            curl_easy_getinfo(P.curl, CURLINFO_STARTTRANSFER_TIME_T, &first);
            curl_easy_getinfo(P.curl, CURLINFO_REDIRECT_COUNT, &redirects);
            curl_easy_getinfo(P.curl, CURLINFO_EFFECTIVE_URL, &eff);
            LOG("timing: dns %lld ms, tcp %lld ms, tls %lld ms, first byte %lld ms, "
                "redirects %ld", (long long)dns / 1000, (long long)tcp / 1000,
                (long long)tls / 1000, (long long)first / 1000, redirects);
            if (redirects && eff)
                LOG("redirected to %s", eff);
        }
    }
    while (done < total && !P.stop) {
        size_t n = sb_write(&P.sb, buf + done, total - done);
        done += n;
        if (n)
            sceKernelSignalLwCond(&P.data_cond);
        if (done < total)
            sceKernelWaitLwCond(&P.space_cond, NULL);
    }
    stopped = P.stop;
    unlock();
    /* Returning less than `total` makes curl abort the transfer. */
    return stopped ? 0 : total;
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

static int net_thread(SceSize args, void *argp)
{
    char errbuf[CURL_ERROR_SIZE] = "";
    CURL *c;
    CURLcode rc;
    (void)args; (void)argp;

    c = curl_easy_init();
    if (!c) {
        lock();
        set_error("Could not start network (curl init failed)");
        sb_set_eof(&P.sb);
        P.net_done = 1;
        sceKernelSignalLwCondAll(&P.data_cond);
        unlock();
        return 0;
    }

    lock();
    P.curl = c;
    unlock();

    curl_easy_setopt(c, CURLOPT_URL, P.url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    /* A live stream that sends nothing for 20 s is treated as dead. */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);

    LOG("net: GET %s", P.url);
    rc = curl_easy_perform(c);

    lock();
    if (!P.user_stop) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (rc == CURLE_OK)
            set_error("Server closed the stream");
        else if (rc == CURLE_HTTP_RETURNED_ERROR)
            set_error("HTTP %ld from server", code);
        else if (rc != CURLE_WRITE_ERROR && rc != CURLE_ABORTED_BY_CALLBACK)
            set_error("%s%s%s", curl_easy_strerror(rc), errbuf[0] ? ": " : "",
                      errbuf);
    }
    LOG("net: finished, curl code %d", (int)rc);
    P.curl = NULL;
    sb_set_eof(&P.sb);
    P.net_done = 1;
    sceKernelSignalLwCondAll(&P.data_cond);
    unlock();

    curl_easy_cleanup(c);
    return 0;
}

/* ---- decode thread ----------------------------------------------------- */

static int16_t pcm[2][GRAIN * 2]; /* double-buffered: hardware reads one */
static unsigned char in_buf[READ_CHUNK];

static int open_port(int port, long rate)
{
    if (port >= 0)
        sceAudioOutReleasePort(port);
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, (int)rate,
                               SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        lock();
        set_error("Audio output refused %ld Hz (0x%08X)", rate, port);
        unlock();
    } else {
        LOG("audio: port %d open at %ld Hz", port, rate);
    }
    return port;
}

static int dec_thread(SceSize args, void *argp)
{
    mpg123_handle *mh;
    const long *rates;
    size_t nrates, i, filled = 0;
    int err, port = -1, cur = 0, failed = 0;
    long port_rate = 0;
    (void)args; (void)argp;

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
        SceUInt64 t0;
        int ret;

        lock();
        while (!P.stop && sb_get_state(&P.sb) == SB_BUFFERING)
            sceKernelWaitLwCond(&P.data_cond, NULL);
        if (P.stop || sb_get_state(&P.sb) == SB_DRAINED) {
            unlock();
            break;
        }
        n = sb_read(&P.sb, in_buf, sizeof(in_buf));
        sceKernelSignalLwCond(&P.space_cond);
        unlock();

        t0 = sceKernelGetProcessTimeWide();
        if (mpg123_feed(mh, in_buf, n) != MPG123_OK) {
            lock();
            set_error("MP3 decoder: %s", mpg123_strerror(mh));
            unlock();
            failed = 1;
            break;
        }

        for (;;) {
            size_t done = 0;
            ret = mpg123_read(mh, (unsigned char *)pcm[cur] + filled,
                              sizeof(pcm[0]) - filled, &done);
            filled += done;

            if (ret == MPG123_NEW_FORMAT) {
                long rate;
                int ch, enc;
                mpg123_getformat(mh, &rate, &ch, &enc);
                LOG("decoder: format %ld Hz, %d ch, enc 0x%x", rate, ch, enc);
                if (rate != port_rate) {
                    port = open_port(port, rate);
                    port_rate = rate;
                    if (port < 0) {
                        failed = 1;
                        break;
                    }
                }
                continue;
            }
            if (ret == MPG123_ERR) {
                lock();
                set_error("MP3 decoder: %s", mpg123_strerror(mh));
                unlock();
                failed = 1;
                break;
            }
            if (filled == sizeof(pcm[0])) {
                lock();
                P.decode_us += sceKernelGetProcessTimeWide() - t0;
                P.samples_out += GRAIN;
                unlock();
                /* Blocks until the port can take another grain. */
                sceAudioOutOutput(port, pcm[cur]);
                t0 = sceKernelGetProcessTimeWide();
                cur ^= 1;
                filled = 0;
            }
            if (ret == MPG123_NEED_MORE)
                break;
        }
        if (failed)
            break;

        {
            struct mpg123_frameinfo fi;
            uint64_t dt = sceKernelGetProcessTimeWide() - t0;
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
    if (port >= 0)
        sceAudioOutReleasePort(port);
    if (mh)
        mpg123_delete(mh);

    lock();
    if (failed) {
        /* Take the network thread down with us. */
        P.stop = 1;
        sceKernelSignalLwCondAll(&P.space_cond);
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
    sceKernelCreateLwMutex(&P.lock, "dxg_player", 0, 0, NULL);
    sceKernelCreateLwCond(&P.data_cond, "dxg_data", 0, &P.lock, NULL);
    sceKernelCreateLwCond(&P.space_cond, "dxg_space", 0, &P.lock, NULL);
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
    P.rate = 0;
    P.channels = P.bitrate_kbps = P.mpeg_version = P.layer = 0;
    P.decode_us = P.samples_out = 0;

    LOG("player: start %s (prebuffer %u KB)", url, (unsigned)(prebuffer / 1024));
    P.net_thid = sceKernelCreateThread("dxg_net", net_thread,
                                       THREAD_PRIO_DEFAULT, NET_STACK, 0, 0, NULL);
    /* Higher priority (lower number) than the UI so audio never starves. */
    P.dec_thid = sceKernelCreateThread("dxg_decode", dec_thread,
                                       THREAD_PRIO_DEFAULT - 20, DEC_STACK, 0, 0,
                                       NULL);
    if (P.net_thid < 0 || P.dec_thid < 0) {
        LOG("player: thread create failed 0x%08X 0x%08X", P.net_thid, P.dec_thid);
        snprintf(P.error, sizeof(P.error), "Could not create player threads");
        if (P.net_thid >= 0)
            sceKernelDeleteThread(P.net_thid);
        if (P.dec_thid >= 0)
            sceKernelDeleteThread(P.dec_thid);
        return;
    }
    P.active = 1;
    sceKernelStartThread(P.net_thid, 0, NULL);
    sceKernelStartThread(P.dec_thid, 0, NULL);
}

static void request_stop(void)
{
    P.stop = 1;
    P.user_stop = 1;
    sceKernelSignalLwCondAll(&P.data_cond);
    sceKernelSignalLwCondAll(&P.space_cond);
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
    sceKernelSignalLwCondAll(&P.data_cond);
    unlock();
}

void player_update(void)
{
    SceUID net, dec;

    lock();
    if (!P.active || !P.net_done || !P.dec_done) {
        unlock();
        return;
    }
    net = P.net_thid;
    dec = P.dec_thid;
    unlock();

    /* Both threads have set their done flag, so these return at once. */
    sceKernelWaitThreadEnd(net, NULL, NULL);
    sceKernelWaitThreadEnd(dec, NULL, NULL);
    sceKernelDeleteThread(net);
    sceKernelDeleteThread(dec);

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
        sceKernelDelayThread(10 * 1000);
    }
    mpg123_exit();
}
