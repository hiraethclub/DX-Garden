/* dxgarden_test: unit tests for core/, built natively on Linux. */
#include <stdio.h>
#include <string.h>

#include "ring_buffer.h"
#include "stream_buffer.h"

#include "test.h"

int failures;
int checks;

static void test_rb_basic(void)
{
    ring_buffer rb;
    char out[16];

    CHECK(rb_init(&rb, 0) == -1);
    CHECK(rb_init(&rb, 8) == 0);
    CHECK(rb_fill(&rb) == 0);
    CHECK(rb_space(&rb) == 8);
    CHECK(rb_read(&rb, out, sizeof(out)) == 0);

    CHECK(rb_write(&rb, "abcde", 5) == 5);
    CHECK(rb_fill(&rb) == 5);
    CHECK(rb_read(&rb, out, 3) == 3);
    CHECK(memcmp(out, "abc", 3) == 0);
    CHECK(rb_fill(&rb) == 2);
    rb_free(&rb);
}

static void test_rb_wrap_and_full(void)
{
    ring_buffer rb;
    char out[16];

    rb_init(&rb, 8);
    rb_write(&rb, "123456", 6);
    rb_read(&rb, out, 4); /* tail at 4, fill 2 */
    /* Wraps past the end: only 6 of 10 bytes fit. */
    CHECK(rb_write(&rb, "abcdefghij", 10) == 6);
    CHECK(rb_fill(&rb) == 8);
    CHECK(rb_space(&rb) == 0);
    CHECK(rb_write(&rb, "x", 1) == 0);
    memset(out, 0, sizeof(out));
    CHECK(rb_read(&rb, out, sizeof(out)) == 8);
    CHECK(memcmp(out, "56abcdef", 8) == 0);
    CHECK(rb_fill(&rb) == 0);

    rb_write(&rb, "zz", 2);
    rb_clear(&rb);
    CHECK(rb_fill(&rb) == 0);
    CHECK(rb_space(&rb) == 8);
    rb_free(&rb);
}

static void test_rb_byte_stream(void)
{
    /* Push a long counting sequence through in odd-sized chunks and check
     * that it comes out intact. */
    ring_buffer rb;
    unsigned char in[37], out[23];
    unsigned next_in = 0, next_out = 0;
    int ok = 1;

    rb_init(&rb, 100);
    while (next_out < 100000) {
        size_t i, n, got;
        for (i = 0; i < sizeof(in); i++)
            in[i] = (unsigned char)(next_in + i);
        n = rb_write(&rb, in, sizeof(in));
        next_in += (unsigned)n;
        got = rb_read(&rb, out, sizeof(out));
        for (i = 0; i < got; i++)
            if (out[i] != (unsigned char)(next_out + i))
                ok = 0;
        next_out += (unsigned)got;
    }
    CHECK(ok);
    rb_free(&rb);
}

static void test_sb_prebuffer(void)
{
    stream_buffer sb;
    char out[64];

    CHECK(sb_init(&sb, 100, 40) == 0);
    CHECK(sb_get_state(&sb) == SB_BUFFERING);
    CHECK(sb_prebuffer_percent(&sb) == 0);

    sb_write(&sb, out, 10);
    CHECK(sb_prebuffer_percent(&sb) == 25);
    CHECK(sb_read(&sb, out, 5) == 0); /* not playable yet */
    CHECK(sb_fill(&sb) == 10);

    sb_write(&sb, out, 30);
    CHECK(sb_get_state(&sb) == SB_PLAYING);
    CHECK(sb_prebuffer_percent(&sb) == 100);
    CHECK(sb_fill_percent(&sb) == 40);
    CHECK(sb_read(&sb, out, 20) == 20);
    CHECK(sb_get_state(&sb) == SB_PLAYING); /* still above zero */
    sb_free(&sb);
}

static void test_sb_underrun(void)
{
    stream_buffer sb;
    char out[64];

    sb_init(&sb, 100, 20);
    sb_write(&sb, out, 20);
    CHECK(sb_read(&sb, out, 64) == 20);
    CHECK(sb_get_state(&sb) == SB_BUFFERING);
    CHECK(sb.underruns == 1);

    /* Needs the full prebuffer again before reads resume. */
    sb_write(&sb, out, 19);
    CHECK(sb_read(&sb, out, 64) == 0);
    sb_write(&sb, out, 1);
    CHECK(sb_get_state(&sb) == SB_PLAYING);
    CHECK(sb_read(&sb, out, 64) == 20);
    CHECK(sb.underruns == 2);
    CHECK(sb.total_in == 40);
    CHECK(sb.total_out == 40);
    sb_free(&sb);
}

static void test_sb_eof(void)
{
    stream_buffer sb;
    char out[64];

    /* EOF before the prebuffer fills: the remainder still plays. */
    sb_init(&sb, 100, 50);
    sb_write(&sb, out, 10);
    sb_set_eof(&sb);
    CHECK(sb_get_state(&sb) == SB_PLAYING);
    CHECK(sb_write(&sb, out, 10) == 0); /* writes refused after EOF */
    CHECK(sb_read(&sb, out, 64) == 10);
    CHECK(sb_get_state(&sb) == SB_DRAINED);
    CHECK(sb.underruns == 0);

    /* EOF on an empty buffer goes straight to DRAINED. */
    sb_reset(&sb);
    CHECK(sb_get_state(&sb) == SB_BUFFERING);
    sb_set_eof(&sb);
    CHECK(sb_get_state(&sb) == SB_DRAINED);
    sb_free(&sb);
}

static void test_sb_prebuffer_clamp(void)
{
    stream_buffer sb;
    char out[8] = {0};

    sb_init(&sb, 50, 500);
    CHECK(sb.prebuffer == 50);
    sb_set_prebuffer(&sb, 0);
    CHECK(sb.prebuffer == 1);
    sb_write(&sb, out, 1);
    CHECK(sb_get_state(&sb) == SB_PLAYING);
    sb_free(&sb);

    /* Lowering the prebuffer below the current fill makes it playable. */
    sb_init(&sb, 50, 40);
    sb_write(&sb, out, 8);
    CHECK(sb_get_state(&sb) == SB_BUFFERING);
    sb_set_prebuffer(&sb, 8);
    CHECK(sb_get_state(&sb) == SB_PLAYING);
    sb_free(&sb);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return check_station_files(argc, argv);

    test_rb_basic();
    test_rb_wrap_and_full();
    test_rb_byte_stream();
    test_sb_prebuffer();
    test_sb_underrun();
    test_sb_eof();
    test_sb_prebuffer_clamp();
    run_library_tests();
    run_globe_tests();
    run_controls_tests();
    run_stationfile_tests();
    run_bidi_tests();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
