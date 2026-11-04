/* Issue #67: unencrypted vs encrypted NVS from the raw pages. The fixtures are the first page
 * of images made by ESP-IDF's nvs_partition_gen.py from the same CSV (namespaces spb, secrets,
 * gw; bdseq u8 41, two strings, a 32-byte blob): "generate" (plain) and "encrypt" (XTS with a
 * generated key, the scheme the firmware's HMAC-derived keys use). */
#include <string.h>

#include "gw_nvsraw.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* first 448 bytes (header, bitmap, 12 entries) of page 0 of plain.bin; the rest is 0xFF */
static const uint8_t k_plain[448] = {
    0xfe, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x84, 0x2d, 0xba, 0xb9,
    0xaa, 0xaa, 0xea, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x01, 0xff, 0x79, 0xd1, 0x2f, 0x51, 0x73, 0x70, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x01, 0x01, 0x01, 0xff, 0xb1, 0xc9, 0x19, 0x7f, 0x62, 0x64, 0x73, 0x65, 0x71, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x01, 0xff, 0x15, 0xc1, 0x1a, 0x28, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x73, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x02, 0x21, 0x02, 0xff, 0x8d, 0x30, 0xfe, 0xd6, 0x63, 0x6f, 0x6d, 0x6d, 0x69, 0x73, 0x73, 0x69,
    0x6f, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x00, 0xff, 0xff, 0xdc, 0x8a, 0x15, 0xbb,
    0x41, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x31, 0x32, 0x33, 0x34, 0x00, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x02, 0x21, 0x02, 0xff, 0x25, 0x4f, 0x71, 0x8b, 0x6d, 0x71, 0x74, 0x74, 0x5f, 0x70, 0x77, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0xff, 0xff, 0xb2, 0x91, 0xb2, 0x68,
    0x73, 0x33, 0x63, 0x72, 0x65, 0x74, 0x2d, 0x6d, 0x71, 0x74, 0x74, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x01, 0xff, 0x4c, 0xd7, 0x5f, 0x1f, 0x67, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x03, 0x42, 0x02, 0x00, 0x65, 0x20, 0xf3, 0xfe, 0x6c, 0x6b, 0x67, 0x5f, 0x6e, 0x65, 0x74, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0xff, 0xff, 0x77, 0x46, 0x13, 0x61,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    0x03, 0x48, 0x01, 0xff, 0xa3, 0xe1, 0xc4, 0xfe, 0x6c, 0x6b, 0x67, 0x5f, 0x6e, 0x65, 0x74, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
/* first 448 bytes (header, bitmap, 12 entries) of page 0 of enc.bin; the rest is 0xFF */
static const uint8_t k_enc[448] = {
    0xfe, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x84, 0x2d, 0xba, 0xb9,
    0xaa, 0xaa, 0xea, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x75, 0x35, 0xde, 0x04, 0x0e, 0xf1, 0xb7, 0xd0, 0xa0, 0x08, 0x9e, 0x37, 0x20, 0xfc, 0xd8, 0x97,
    0xde, 0x15, 0xb7, 0x25, 0x02, 0x08, 0xab, 0x2a, 0x52, 0x8b, 0x71, 0x60, 0xfa, 0x94, 0xcc, 0x5c,
    0xa4, 0xb0, 0x36, 0x7c, 0x18, 0x25, 0x73, 0xc8, 0x45, 0xe0, 0xb4, 0x64, 0xf4, 0x10, 0x62, 0xfc,
    0x0c, 0x1b, 0x89, 0xdd, 0xca, 0x6a, 0xc0, 0xc3, 0x62, 0xc3, 0xa8, 0x7f, 0x2b, 0xe9, 0xf5, 0x50,
    0x42, 0x24, 0x26, 0x20, 0x07, 0x9d, 0xe2, 0xdb, 0x42, 0x65, 0x7b, 0x3d, 0x71, 0xb1, 0xec, 0xef,
    0x12, 0x07, 0x16, 0x17, 0xb6, 0xbd, 0xae, 0x2f, 0x23, 0x69, 0xba, 0x1b, 0x4a, 0x9f, 0x05, 0xd9,
    0xf7, 0xa9, 0xb5, 0x0a, 0x86, 0x4a, 0xb8, 0xab, 0x86, 0x75, 0xff, 0x6e, 0x93, 0x12, 0xd6, 0x90,
    0xc1, 0xf1, 0xaa, 0x8d, 0x0f, 0x50, 0x9b, 0xfb, 0xb1, 0x57, 0x63, 0x3b, 0x15, 0xf1, 0xc7, 0x22,
    0xb4, 0x4a, 0x58, 0x0a, 0x99, 0xb7, 0x10, 0x74, 0x83, 0xe2, 0x23, 0x59, 0x92, 0x24, 0x68, 0x92,
    0x61, 0x30, 0x7b, 0xcc, 0x99, 0x47, 0xd2, 0x06, 0x6c, 0x76, 0x47, 0xc8, 0xc6, 0x86, 0xf3, 0x18,
    0x67, 0xd3, 0x48, 0xfb, 0xef, 0xd6, 0xe0, 0x5f, 0xe6, 0xae, 0x75, 0x15, 0xde, 0x68, 0x1d, 0x68,
    0x97, 0xa9, 0xdd, 0xb6, 0x9e, 0x0b, 0xe8, 0xb8, 0xee, 0xcf, 0xd4, 0xe0, 0xdf, 0xaa, 0x6b, 0x13,
    0xdf, 0x9d, 0x0e, 0x85, 0x28, 0x4b, 0x58, 0x8f, 0xd9, 0x84, 0xd2, 0xbd, 0x39, 0x65, 0xb4, 0x9a,
    0x4f, 0x9b, 0x37, 0x2c, 0x4f, 0xb0, 0x85, 0xaa, 0x15, 0x16, 0xbd, 0xa8, 0x63, 0x9c, 0xf2, 0xcc,
    0x32, 0xa1, 0x2d, 0xaa, 0x58, 0x31, 0xd4, 0xc1, 0x0c, 0x57, 0xae, 0xcd, 0xf1, 0x5d, 0x2e, 0x58,
    0x56, 0x43, 0x51, 0x3e, 0x0e, 0x6d, 0x21, 0x73, 0x2a, 0xea, 0xd9, 0x60, 0xdf, 0x7a, 0x3e, 0x65,
    0x4d, 0x10, 0x7c, 0x83, 0x1f, 0xc5, 0x75, 0x2c, 0x66, 0x4c, 0x06, 0xa9, 0x46, 0x6d, 0x1a, 0xd9,
    0x25, 0x03, 0xcf, 0x2b, 0x14, 0xa7, 0xb8, 0x10, 0x86, 0xe4, 0xd6, 0x4c, 0x8e, 0x81, 0x25, 0xba,
    0x6b, 0xc0, 0xe3, 0x44, 0x54, 0x86, 0xff, 0xc4, 0x4f, 0xd9, 0x9d, 0x48, 0x03, 0x51, 0x06, 0x87,
    0x89, 0xb7, 0x02, 0xf7, 0x52, 0xef, 0x0c, 0x83, 0xf4, 0xdd, 0xac, 0x1d, 0x02, 0xda, 0xfa, 0x85,
    0xd0, 0xd8, 0x14, 0x9a, 0x96, 0xb0, 0x4c, 0xe6, 0xe7, 0xc6, 0xa6, 0x4c, 0x03, 0x40, 0x07, 0xa6,
    0x87, 0x48, 0x9b, 0xd1, 0x26, 0x26, 0xde, 0x26, 0xc2, 0x17, 0xd3, 0xe2, 0xcd, 0x95, 0x88, 0x72,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static uint8_t page[GW_NVSRAW_PAGE];

static const uint8_t *load(const uint8_t *prefix)
{
    memset(page, 0xFF, sizeof(page));
    memcpy(page, prefix, 448);
    return page;
}

static gw_nvsraw_kind_t scan(const uint8_t *p, gw_nvsraw_scan_t *s)
{
    gw_nvsraw_scan_init(s);
    gw_nvsraw_scan_page(s, p);
    return gw_nvsraw_verdict(s);
}

static void test_crc_matches_esp_rom(void)
{
    /* esp_rom_crc32_le(0, "123456789", 9) is the standard CRC-32: 0xCBF43926 */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, gw_nvsraw_crc32(0, (const uint8_t *)"123456789", 9));
    /* in two pieces, as NVS chains it */
    uint32_t c = gw_nvsraw_crc32(0, (const uint8_t *)"1234", 4);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, gw_nvsraw_crc32(c, (const uint8_t *)"56789", 5));
}

static void test_plain_image(void)
{
    gw_nvsraw_scan_t s;
    TEST_ASSERT_EQUAL(GW_NVSRAW_PLAIN, scan(load(k_plain), &s));
    TEST_ASSERT_EQUAL_UINT32(1, s.pages);
    /* 3 namespace entries, bdseq, 2 strings and the blob (data + index): 8 items */
    TEST_ASSERT_EQUAL_UINT32(8, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(0, s.bad);
    TEST_ASSERT_TRUE(s.written > s.plain_ok);   /* string and blob data spans */
}

static void test_encrypted_image(void)
{
    gw_nvsraw_scan_t s;
    TEST_ASSERT_EQUAL(GW_NVSRAW_ENCRYPTED, scan(load(k_enc), &s));
    TEST_ASSERT_EQUAL_UINT32(1, s.pages);
    TEST_ASSERT_EQUAL_UINT32(0, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(s.written, s.bad);
    TEST_ASSERT_TRUE(s.written >= 8);
}

static void test_erased_or_empty(void)
{
    gw_nvsraw_scan_t s;
    memset(page, 0xFF, sizeof(page));            /* never initialised */
    TEST_ASSERT_EQUAL(GW_NVSRAW_EMPTY, scan(page, &s));
    TEST_ASSERT_EQUAL_UINT32(0, s.pages);
    /* an active page whose entries were all erased (bitmap 00) */
    load(k_plain);
    memset(page + 32, 0x00, 32);
    TEST_ASSERT_EQUAL(GW_NVSRAW_EMPTY, scan(page, &s));
    TEST_ASSERT_EQUAL_UINT32(1, s.pages);
    /* a bad header CRC: not an NVS page */
    load(k_plain);
    page[4] ^= 1;
    TEST_ASSERT_EQUAL(GW_NVSRAW_EMPTY, scan(page, &s));
    TEST_ASSERT_EQUAL_UINT32(0, s.pages);
}

static void test_one_corrupt_entry_keeps_plain(void)
{
    gw_nvsraw_scan_t s;
    load(k_plain);
    page[64 + 32 + 10] ^= 0x40;                  /* the key of the second entry */
    TEST_ASSERT_EQUAL(GW_NVSRAW_PLAIN, scan(page, &s));
    TEST_ASSERT_EQUAL_UINT32(1, s.bad);
}

static void test_several_pages_accumulate(void)
{
    gw_nvsraw_scan_t s;
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, load(k_enc));
    gw_nvsraw_scan_page(&s, load(k_enc));
    memset(page, 0xFF, sizeof(page));
    gw_nvsraw_scan_page(&s, page);
    TEST_ASSERT_EQUAL_UINT32(2, s.pages);
    TEST_ASSERT_EQUAL(GW_NVSRAW_ENCRYPTED, gw_nvsraw_verdict(&s));
}

/* ---------- issue #94 ---------- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* page sequence number, with the header CRC fixed */
static void set_seq(uint8_t *pg, uint32_t seq)
{
    wr32(pg + 4, seq);
    wr32(pg + 28, gw_nvsraw_crc32(0xFFFFFFFFu, pg + 4, 24));
}

/* first data byte of entry @p i, with the item CRC fixed */
static void set_u8(uint8_t *pg, unsigned i, uint8_t v)
{
    uint8_t *e = pg + 64 + 32 * i;
    e[24] = v;
    uint32_t c = gw_nvsraw_crc32(0xFFFFFFFFu, e, 4);
    c = gw_nvsraw_crc32(c, e + 8, 16);
    wr32(e + 4, gw_nvsraw_crc32(c, e + 24, 8));
}

static void test_mixed_pages(void)
{
    static uint8_t enc[GW_NVSRAW_PAGE];
    memcpy(enc, load(k_enc), sizeof(enc));
    gw_nvsraw_scan_t s;
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, enc);
    gw_nvsraw_scan_page(&s, load(k_plain));
    TEST_ASSERT_EQUAL_UINT32(1, s.plain_pages);
    TEST_ASSERT_EQUAL_UINT32(1, s.enc_pages);
    TEST_ASSERT_EQUAL(GW_NVSRAW_MIXED, gw_nvsraw_verdict(&s));
    /* the order of the pages does not matter */
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, load(k_plain));
    gw_nvsraw_scan_page(&s, enc);
    TEST_ASSERT_EQUAL(GW_NVSRAW_MIXED, gw_nvsraw_verdict(&s));
    /* a plain page whose entries all fail their CRC counts as encrypted: with a plain one, mixed */
    static uint8_t broken[GW_NVSRAW_PAGE];
    memcpy(broken, load(k_plain), sizeof(broken));
    for (unsigned i = 0; i < 12; i++) {
        broken[64 + 32 * i + 4] ^= 0x5A;
    }
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, broken);
    TEST_ASSERT_EQUAL(GW_NVSRAW_ENCRYPTED, gw_nvsraw_verdict(&s));
    gw_nvsraw_scan_page(&s, load(k_plain));
    TEST_ASSERT_EQUAL(GW_NVSRAW_MIXED, gw_nvsraw_verdict(&s));
    /* plain pages only (one with a corrupt entry): plain */
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, load(k_plain));
    page[64 + 32 + 10] ^= 0x40;
    gw_nvsraw_scan_page(&s, page);
    TEST_ASSERT_EQUAL_UINT32(2, s.plain_pages);
    TEST_ASSERT_EQUAL(GW_NVSRAW_PLAIN, gw_nvsraw_verdict(&s));
}

static void test_raw_u8_bdseq(void)
{
    gw_nvsraw_u8_t f;
    gw_nvsraw_u8_init(&f, "spb", "bdseq");
    gw_nvsraw_u8_ns(&f, load(k_plain));
    TEST_ASSERT_EQUAL_INT(1, f.ns_index);
    gw_nvsraw_u8_item(&f, page);
    TEST_ASSERT_TRUE(f.found);
    TEST_ASSERT_EQUAL_UINT8(41, f.value);
    /* another namespace's key of the same name is not it; a missing key is not found */
    gw_nvsraw_u8_t g;
    gw_nvsraw_u8_init(&g, "gw", "bdseq");
    gw_nvsraw_u8_ns(&g, page);
    gw_nvsraw_u8_item(&g, page);
    TEST_ASSERT_EQUAL_INT(3, g.ns_index);
    TEST_ASSERT_FALSE(g.found);
    /* nothing in an encrypted page */
    gw_nvsraw_u8_init(&g, "spb", "bdseq");
    gw_nvsraw_u8_ns(&g, load(k_enc));
    gw_nvsraw_u8_item(&g, page);
    TEST_ASSERT_EQUAL_INT(-1, g.ns_index);
    TEST_ASSERT_FALSE(g.found);
    /* two written copies (a reset mid-update): the page with the higher sequence wins, in
     * either order */
    static uint8_t newer[GW_NVSRAW_PAGE];
    memcpy(newer, load(k_plain), sizeof(newer));
    set_seq(newer, 7);
    set_u8(newer, 1, 42);
    static uint8_t old[GW_NVSRAW_PAGE];
    memcpy(old, load(k_plain), sizeof(old));
    for (int order = 0; order < 2; order++) {
        gw_nvsraw_u8_init(&f, "spb", "bdseq");
        const uint8_t *first = order ? newer : old;
        const uint8_t *second = order ? old : newer;
        gw_nvsraw_u8_ns(&f, first);
        gw_nvsraw_u8_ns(&f, second);
        gw_nvsraw_u8_item(&f, first);
        gw_nvsraw_u8_item(&f, second);
        TEST_ASSERT_TRUE(f.found);
        TEST_ASSERT_EQUAL_UINT8(42, f.value);
        TEST_ASSERT_EQUAL_UINT32(7, f.seq);
    }
    /* a value whose CRC does not match is not taken */
    set_u8(newer, 1, 43);
    newer[64 + 32 + 24] = 44;
    gw_nvsraw_u8_init(&f, "spb", "bdseq");
    gw_nvsraw_u8_ns(&f, newer);
    gw_nvsraw_u8_item(&f, newer);
    TEST_ASSERT_FALSE(f.found);
    TEST_ASSERT_EQUAL_UINT32(7, rd32(newer + 4));
}

static void test_copy_round_trip(void)
{
    static uint8_t buf[512];
    size_t n = gw_nvsmig_begin(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(GW_NVSMIG_HDR, n);
    uint8_t b = 41;
    const char *pw = "s3cret-mqtt-67";
    uint8_t blob[32];
    for (unsigned i = 0; i < sizeof(blob); i++) {
        blob[i] = (uint8_t)i;
    }
    n = gw_nvsmig_add(buf, sizeof(buf), n, "spb", "bdseq", 0x01, &b, 1);
    n = gw_nvsmig_add(buf, sizeof(buf), n, "secrets", "mqtt_pw", 0x21, pw, strlen(pw) + 1);
    n = gw_nvsmig_add(buf, sizeof(buf), n, "gw", "lkg_net", 0x42, blob, sizeof(blob));
    TEST_ASSERT_EQUAL_size_t(GW_NVSMIG_HDR + 3 * GW_NVSMIG_REC_HDR + 1 + 15 + 32, n);
    /* found before it is sealed (the bdSeq fallback looks for it) */
    gw_nvsmig_rec_t r;
    TEST_ASSERT_TRUE(gw_nvsmig_find(buf, n, "spb", "bdseq", &r));
    TEST_ASSERT_FALSE(gw_nvsmig_check(buf, n, NULL));   /* not sealed yet */
    size_t size = gw_nvsmig_seal(buf, n, 2);
    uint32_t count = 0;
    TEST_ASSERT_TRUE(gw_nvsmig_check(buf, size, &count));
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_UINT16(2, gw_nvsmig_skipped(buf));   /* what the migration could not copy */
    TEST_ASSERT_EQUAL_HEX32(gw_nvsraw_crc32(0, buf + GW_NVSMIG_HDR, size - GW_NVSMIG_HDR),
                            gw_nvsmig_crc(buf));
    size_t off = 0;
    TEST_ASSERT_TRUE(gw_nvsmig_next(buf, size, &off, &r));
    TEST_ASSERT_EQUAL_STRING("spb", r.ns);
    TEST_ASSERT_EQUAL_STRING("bdseq", r.key);
    TEST_ASSERT_EQUAL_UINT8(0x01, r.type);
    TEST_ASSERT_EQUAL_UINT16(1, r.len);
    TEST_ASSERT_EQUAL_UINT8(41, r.data[0]);
    TEST_ASSERT_TRUE(gw_nvsmig_next(buf, size, &off, &r));
    TEST_ASSERT_EQUAL_STRING("mqtt_pw", r.key);
    TEST_ASSERT_EQUAL_STRING(pw, (const char *)r.data);
    TEST_ASSERT_TRUE(gw_nvsmig_next(buf, size, &off, &r));
    TEST_ASSERT_EQUAL_UINT8(0x42, r.type);
    TEST_ASSERT_EQUAL_MEMORY(blob, r.data, sizeof(blob));
    TEST_ASSERT_FALSE(gw_nvsmig_next(buf, size, &off, &r));
    TEST_ASSERT_FALSE(gw_nvsmig_find(buf, size, "spb", "nope", &r));

    /* damaged copies are refused: a flipped byte, cut short, longer, another magic */
    buf[GW_NVSMIG_HDR + 40] ^= 1;
    TEST_ASSERT_FALSE(gw_nvsmig_check(buf, size, NULL));
    buf[GW_NVSMIG_HDR + 40] ^= 1;
    TEST_ASSERT_TRUE(gw_nvsmig_check(buf, size, NULL));
    TEST_ASSERT_FALSE(gw_nvsmig_check(buf, size - 1, NULL));
    TEST_ASSERT_FALSE(gw_nvsmig_check(buf, size + 1, NULL));
    TEST_ASSERT_FALSE(gw_nvsmig_check(buf, 3, NULL));
    buf[0] ^= 1;
    TEST_ASSERT_FALSE(gw_nvsmig_check(buf, size, NULL));
    buf[0] ^= 1;

    /* an empty copy is valid */
    static uint8_t e[GW_NVSMIG_HDR];
    TEST_ASSERT_EQUAL_size_t(0, gw_nvsmig_begin(e, sizeof(e) - 1));
    size = gw_nvsmig_seal(e, gw_nvsmig_begin(e, sizeof(e)), 0);
    TEST_ASSERT_TRUE(gw_nvsmig_check(e, size, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);

    /* no room, names too long or empty: nothing appended */
    n = gw_nvsmig_begin(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(0, gw_nvsmig_add(buf, GW_NVSMIG_HDR + GW_NVSMIG_REC_HDR, n, "spb",
                                              "bdseq", 0x01, &b, 1));
    TEST_ASSERT_EQUAL_size_t(0, gw_nvsmig_add(buf, sizeof(buf), n, "spb",
                                              "a-key-of-16-char", 0x01, &b, 1));
    TEST_ASSERT_EQUAL_size_t(0, gw_nvsmig_add(buf, sizeof(buf), n, "", "bdseq", 0x01, &b, 1));
    TEST_ASSERT_NOT_EQUAL(0, gw_nvsmig_add(buf, sizeof(buf), n, "spb", "a-key-of-15-chr", 0x01,
                                           &b, 1));
}

static gw_nvsmig_action_t decide(gw_nvsraw_kind_t kind, gw_nvsmig_copy_t copy, bool trial,
                                 bool can_copy, bool raw_ok)
{
    const gw_nvsmig_state_t st = {.kind = kind, .raw_ok = raw_ok, .copy = copy,
                                  .on_trial = trial, .can_copy = can_copy};
    return gw_nvsmig_decide(&st);
}

static const gw_nvsraw_kind_t kKinds[] = {GW_NVSRAW_EMPTY, GW_NVSRAW_PLAIN, GW_NVSRAW_ENCRYPTED,
                                          GW_NVSRAW_MIXED};

static void test_decide(void)
{
    for (int c = 0; c < 2; c++) {   /* no copy, or a damaged one (read whole, deleted) */
        gw_nvsmig_copy_t copy = c ? GW_NVSMIG_COPY_BAD : GW_NVSMIG_COPY_NONE;
        /* unencrypted (or mixed: the plain entries are the newer ones) on a confirmed image */
        TEST_ASSERT_EQUAL(GW_NVSMIG_MIGRATE, decide(GW_NVSRAW_PLAIN, copy, false, true, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_MIGRATE, decide(GW_NVSRAW_MIXED, copy, false, true, true));
        /* on trial: a rollback must find what the previous image can read */
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, decide(GW_NVSRAW_PLAIN, copy, true, true, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, decide(GW_NVSRAW_MIXED, copy, true, true, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, decide(GW_NVSRAW_EMPTY, copy, true, true, true));
        /* nowhere to keep the copy: nothing is erased */
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, decide(GW_NVSRAW_PLAIN, copy, false, false, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, decide(GW_NVSRAW_MIXED, copy, false, false, true));
        /* already encrypted (also on trial), or empty on a confirmed image */
        for (int t = 0; t < 2; t++) {
            for (int f = 0; f < 2; f++) {
                TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED,
                                  decide(GW_NVSRAW_ENCRYPTED, copy, t, f, true));
            }
        }
        TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, decide(GW_NVSRAW_EMPTY, copy, false, true, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, decide(GW_NVSRAW_EMPTY, copy, false, false, true));
    }
}

/* Round 2 of issue #94: a valid copy, an unreadable copy, unreadable raw pages. */
static void test_decide_with_copy(void)
{
    const gw_nvsmig_copy_t V = GW_NVSMIG_COPY_VALID;
    /* the erase was done (empty or encrypted pages): the copy is all there is, also on trial */
    for (int t = 0; t < 2; t++) {
        TEST_ASSERT_EQUAL(GW_NVSMIG_RESUME, decide(GW_NVSRAW_EMPTY, V, t, true, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_RESUME, decide(GW_NVSRAW_ENCRYPTED, V, t, true, true));
    }
    /* the erase had not finished: the partition's entries first, the copy fills in */
    TEST_ASSERT_EQUAL(GW_NVSMIG_MIGRATE, decide(GW_NVSRAW_PLAIN, V, false, true, true));
    TEST_ASSERT_EQUAL(GW_NVSMIG_MIGRATE, decide(GW_NVSRAW_MIXED, V, false, true, true));
    /* B1(a), N2: a new image on trial never migrates, and with a pending copy (a partly erased
     * partition) it does not run on the partition either: no NVS until the confirmed boot */
    TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, decide(GW_NVSRAW_PLAIN, V, true, true, true));
    TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, decide(GW_NVSRAW_MIXED, V, true, true, true));
    TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, decide(GW_NVSRAW_PLAIN, V, false, false, true));
    TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, decide(GW_NVSRAW_MIXED, V, false, false, true));
    /* M2: a copy that exists but could not be read whole: no NVS this boot, whatever else */
    for (unsigned k = 0; k < 4; k++) {
        for (int t = 0; t < 2; t++) {
            for (int r = 0; r < 2; r++) {
                TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS,
                                  decide(kKinds[k], GW_NVSMIG_COPY_UNREADABLE, t, true, r));
            }
        }
    }
    /* B2: a raw page not read: never a migration nor a resume */
    for (unsigned k = 0; k < 4; k++) {
        for (int t = 0; t < 2; t++) {
            TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, decide(kKinds[k], V, t, true, false));
            for (int c = 0; c < 2; c++) {
                gw_nvsmig_copy_t copy = c ? GW_NVSMIG_COPY_BAD : GW_NVSMIG_COPY_NONE;
                gw_nvsmig_action_t a = decide(kKinds[k], copy, t, true, false);
                /* N1: only readable pages of one kind say what the partition is; nothing
                 * readable (EMPTY: also when the page buffer was not allocated) or both kinds:
                 * no NVS, never a plain init over what may be an encrypted NVS */
                gw_nvsmig_action_t want = kKinds[k] == GW_NVSRAW_ENCRYPTED ? GW_NVSMIG_OPEN_ENCRYPTED
                                          : kKinds[k] == GW_NVSRAW_PLAIN   ? GW_NVSMIG_STAY_PLAIN
                                                                           : GW_NVSMIG_NO_NVS;
                TEST_ASSERT_EQUAL(want, a);
            }
        }
    }
}

/* M1: the partition is erased only when every entry is in the copy */
static void test_may_erase(void)
{
    gw_nvsmig_read_t r = {.init_ok = true, .listed_all = true, .skipped = 0};
    TEST_ASSERT_TRUE(gw_nvsmig_may_erase(&r));
    r.skipped = 1;                          /* one entry not copied (no memory for its value) */
    TEST_ASSERT_FALSE(gw_nvsmig_may_erase(&r));
    r.skipped = 0;
    r.listed_all = false;                   /* the iteration stopped with another error */
    TEST_ASSERT_FALSE(gw_nvsmig_may_erase(&r));
    /* the unencrypted NVS does not start: only for a layout error (IDF erases it anyway) */
    gw_nvsmig_read_t f = {.init_ok = false, .init_layout_error = false};
    TEST_ASSERT_FALSE(gw_nvsmig_may_erase(&f));   /* ESP_ERR_NO_MEM, a flash read error, ... */
    f.init_layout_error = true;                   /* NO_FREE_PAGES, NEW_VERSION_FOUND */
    TEST_ASSERT_TRUE(gw_nvsmig_may_erase(&f));
}

/* Issue #104: LittleFS does not mount (a copy may be in it) and the partition looks like an
 * interrupted migration: no NVS, rather than an empty or partial encrypted one */
static void test_decide_copy_out_of_reach(void)
{
    gw_nvsmig_state_t st = {.kind = GW_NVSRAW_EMPTY, .raw_ok = true, .copy = GW_NVSMIG_COPY_NONE,
                            .can_copy = false, .copy_unknown = true, .keys = true};
    for (int t = 0; t < 2; t++) {
        st.on_trial = t;
        st.kind = GW_NVSRAW_EMPTY;   /* cut right after the erase: the keys were made before */
        TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, gw_nvsmig_decide(&st));
        st.kind = GW_NVSRAW_ENCRYPTED;   /* cut while writing: started, not finished */
        st.mig_busy = true;
        TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, gw_nvsmig_decide(&st));
        st.raw_ok = false;
        TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, gw_nvsmig_decide(&st));
        st.raw_ok = true;
        st.mig_busy = false;   /* a finished (or no) migration: the encrypted NVS opens */
        TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, gw_nvsmig_decide(&st));
    }
    /* EMPTY without the keys: a device that never had an encrypted NVS (no migration started) */
    st.on_trial = false;
    st.kind = GW_NVSRAW_EMPTY;
    st.keys = false;
    TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, gw_nvsmig_decide(&st));
    /* a blank LittleFS partition (erased: no copy can be there) is not out of reach */
    st.keys = true;
    st.copy_unknown = false;
    TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, gw_nvsmig_decide(&st));
    st.kind = GW_NVSRAW_ENCRYPTED;
    st.mig_busy = true;
    TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, gw_nvsmig_decide(&st));
    /* issue #108 B2: an unencrypted partition with the keys present may be partly erased (cut
     * during the erase): no NVS either; without the keys no migration ever started */
    st.copy_unknown = true;
    for (int t = 0; t < 2; t++) {
        st.on_trial = t;
        st.kind = GW_NVSRAW_PLAIN;
        TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, gw_nvsmig_decide(&st));
        st.kind = GW_NVSRAW_MIXED;
        TEST_ASSERT_EQUAL(GW_NVSMIG_NO_NVS, gw_nvsmig_decide(&st));
    }
    st.on_trial = false;
    st.keys = false;
    st.kind = GW_NVSRAW_PLAIN;
    TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(&st));
    st.on_trial = true;
    TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(&st));
    /* the same, LittleFS blank: nothing to protect, as before */
    st.keys = true;
    st.copy_unknown = false;
    st.on_trial = false;
    TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(&st));   /* no place for a copy */
}

/* Issue #104: the reservation of bdSeq values of a boot without NVS */
static void test_bdseq_reservation(void)
{
    TEST_ASSERT_FALSE(gw_nvsmig_bdseq_reached(41, 57));
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_reached(57, 57));
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_reached(58, 57));   /* never skipped past unnoticed */
    TEST_ASSERT_FALSE(gw_nvsmig_bdseq_reached(250, 10));   /* across the wrap */
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_reached(10, 10));
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_reached(12, 250));
    /* the next boot with NVS continues from the reservation, never below what was handed out */
    for (unsigned start = 0; start < 256; start++) {
        uint8_t reserved = (uint8_t)(start + GW_NVSMIG_BDSEQ_BLOCK), v = (uint8_t)start;
        for (unsigned i = 0; i < 3 * GW_NVSMIG_BDSEQ_BLOCK; i++) {
            v++;   /* handed out */
            if (gw_nvsmig_bdseq_reached(v, reserved)) {
                reserved = (uint8_t)(v + GW_NVSMIG_BDSEQ_BLOCK);
            }
            TEST_ASSERT_EQUAL_UINT8(reserved, gw_nvsmig_bdseq_later(v, reserved));
        }
    }
}

/* Issue #108 B3, the review's simulation against the code: boots without NVS whose pending copy
 * (bdSeq 10) never gets applied, one session each. The chain of reservations continues from its
 * base however far it went (it went back to 10 at the tenth boot before); the first boot with NVS
 * continues after it. */
static void test_bdseq_reservation_chain(void)
{
    const uint8_t c = 10;
    gw_nvsmig_side_t side = {0};
    uint8_t prev = 0;
    for (int boot = 1; boot <= 40; boot++) {
        uint8_t start, base;
        TEST_ASSERT_TRUE(gw_nvsmig_bdseq_start(true, c, &side, &start, &base));
        if (boot > 1) {   /* one value per boot, then the next block */
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(prev + GW_NVSMIG_BDSEQ_BLOCK), start);
        }
        TEST_ASSERT_EQUAL_UINT8(c, base);
        prev = start;
        side = (gw_nvsmig_side_t){.have = true, .base_known = true, .base = base,
                                  .next = (uint8_t)(start + GW_NVSMIG_BDSEQ_BLOCK)};
    }
    /* the migration finally writes the copy (10): the NVS goes on from the reservation */
    TEST_ASSERT_EQUAL_UINT8(side.next, gw_nvsmig_bdseq_fold(true, c, &side));
    /* an NVS that moved on by itself (another base): the later of both */
    TEST_ASSERT_EQUAL_UINT8(side.next, gw_nvsmig_bdseq_fold(true, (uint8_t)(side.next - 3), &side));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(side.next + 5),
                            gw_nvsmig_bdseq_fold(true, (uint8_t)(side.next + 5), &side));
    TEST_ASSERT_EQUAL_UINT8(side.next, gw_nvsmig_bdseq_fold(false, 0, &side));
    /* no reservation: the NVS's own; nothing known and no reservation: random (false) */
    gw_nvsmig_side_t none = {0};
    TEST_ASSERT_EQUAL_UINT8(77, gw_nvsmig_bdseq_fold(true, 77, &none));
    uint8_t start, base;
    TEST_ASSERT_FALSE(gw_nvsmig_bdseq_start(false, 0, &none, &start, &base));
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_start(true, 42, &none, &start, &base));
    TEST_ASSERT_EQUAL_UINT8(42, start);
    /* a one-byte reservation of #104 (no base): the later, as before */
    gw_nvsmig_side_t old = {.have = true, .base_known = false, .next = 60};
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_start(true, 50, &old, &start, &base));
    TEST_ASSERT_EQUAL_UINT8(60, start);
    TEST_ASSERT_EQUAL_UINT8(50, base);
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_start(false, 0, &old, &start, &base));
    TEST_ASSERT_EQUAL_UINT8(60, start);
    /* a known value newer than the reservation's chain (a resume moved the copy on) */
    gw_nvsmig_side_t s2 = {.have = true, .base_known = true, .base = 10, .next = 40};
    TEST_ASSERT_TRUE(gw_nvsmig_bdseq_start(true, 45, &s2, &start, &base));
    TEST_ASSERT_EQUAL_UINT8(45, start);
    TEST_ASSERT_EQUAL_UINT8(45, base);
}

static void test_bdseq_later(void)
{
    TEST_ASSERT_EQUAL_UINT8(42, gw_nvsmig_bdseq_later(41, 42));
    TEST_ASSERT_EQUAL_UINT8(42, gw_nvsmig_bdseq_later(42, 41));
    TEST_ASSERT_EQUAL_UINT8(7, gw_nvsmig_bdseq_later(7, 7));
    TEST_ASSERT_EQUAL_UINT8(2, gw_nvsmig_bdseq_later(250, 2));    /* across the wrap */
    TEST_ASSERT_EQUAL_UINT8(2, gw_nvsmig_bdseq_later(2, 250));
    TEST_ASSERT_EQUAL_UINT8(127, gw_nvsmig_bdseq_later(0, 127));
    /* half the ring: undecidable; the same answer in either order (N3) */
    TEST_ASSERT_EQUAL_UINT8(128, gw_nvsmig_bdseq_later(0, 128));
    TEST_ASSERT_EQUAL_UINT8(128, gw_nvsmig_bdseq_later(128, 0));
    TEST_ASSERT_EQUAL_UINT8(200, gw_nvsmig_bdseq_later(72, 200));
    TEST_ASSERT_EQUAL_UINT8(200, gw_nvsmig_bdseq_later(200, 72));
    for (unsigned a = 0; a < 256; a += 7) {
        for (unsigned b = 0; b < 256; b += 5) {
            TEST_ASSERT_EQUAL_UINT8(gw_nvsmig_bdseq_later((uint8_t)a, (uint8_t)b),
                                    gw_nvsmig_bdseq_later((uint8_t)b, (uint8_t)a));
        }
    }
}

/* N5: the copy file as read: current version, version 1 (converted), unknown version (kept),
 * not a copy or damaged (the only cases to delete it) */
static void test_copy_load(void)
{
    static uint8_t buf[256], v1[256];
    uint8_t b = 41;
    size_t n = gw_nvsmig_begin(buf, sizeof(buf));
    n = gw_nvsmig_add(buf, sizeof(buf), n, "spb", "bdseq", 0x01, &b, 1);
    n = gw_nvsmig_add(buf, sizeof(buf), n, "secrets", "mqtt_pw", 0x21, "pw", 3);
    size_t size = gw_nvsmig_seal(buf, n, 0);
    size_t out = 0;
    bool legacy = true;
    uint32_t count = 0;
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_VALID, gw_nvsmig_load(buf, size, sizeof(buf), &out, &legacy,
                                                           &count));
    TEST_ASSERT_EQUAL_size_t(size, out);
    TEST_ASSERT_FALSE(legacy);
    TEST_ASSERT_EQUAL_UINT32(2, count);

    /* the same copy as ebc2aa4 wrote it: 16-byte header, version 1 */
    size_t body = size - GW_NVSMIG_HDR;
    memcpy(v1, buf, 16);
    v1[4] = 1;
    v1[5] = 0;
    memcpy(v1 + 16, buf + GW_NVSMIG_HDR, body);
    wr32(v1 + 8, (uint32_t)body);
    wr32(v1 + 12, gw_nvsraw_crc32(0, v1 + 16, body));
    static uint8_t w[256];
    memcpy(w, v1, sizeof(w));
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_VALID, gw_nvsmig_load(w, 16 + body, sizeof(w), &out, &legacy,
                                                           &count));
    TEST_ASSERT_TRUE(legacy);
    TEST_ASSERT_EQUAL_size_t(size, out);
    TEST_ASSERT_EQUAL_MEMORY(buf, w, size);   /* converted into the current version */
    /* valid v1, but no room to convert it: kept */
    memcpy(w, v1, sizeof(w));
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_UNREADABLE, gw_nvsmig_load(w, 16 + body, 16 + body, &out,
                                                                &legacy, &count));
    /* damaged v1: deleted */
    memcpy(w, v1, sizeof(w));
    w[16 + 3] ^= 1;
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_BAD, gw_nvsmig_load(w, 16 + body, sizeof(w), &out, &legacy,
                                                         &count));
    /* an unknown (newer) version: kept, never deleted */
    memcpy(w, buf, size);
    w[4] = 3;
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_UNREADABLE, gw_nvsmig_load(w, size, sizeof(w), &out, &legacy,
                                                                &count));
    /* not a copy, or too short to say: deleted; a damaged current one too */
    memcpy(w, buf, size);
    w[0] ^= 1;
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_BAD, gw_nvsmig_load(w, size, sizeof(w), &out, &legacy, &count));
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_BAD, gw_nvsmig_load(buf, 3, sizeof(buf), &out, &legacy, &count));
    memcpy(w, buf, size);
    w[GW_NVSMIG_HDR + 2] ^= 1;
    TEST_ASSERT_EQUAL(GW_NVSMIG_COPY_BAD, gw_nvsmig_load(w, size, sizeof(w), &out, &legacy, &count));
}

/* ---------- issue #97: what the review found untested ---------- */

/* entry @p i of @p pg: a copy of the 32-byte entry @p src with item span @p span, CRC fixed, and
 * marked written in the state bitmap */
static void put_entry(uint8_t *pg, unsigned i, const uint8_t *src, uint8_t span)
{
    uint8_t *e = pg + 64 + 32 * i;
    memcpy(e, src, 32);
    e[2] = span;
    uint32_t c = gw_nvsraw_crc32(0xFFFFFFFFu, e, 4);
    c = gw_nvsraw_crc32(c, e + 8, 16);
    wr32(e + 4, gw_nvsraw_crc32(c, e + 24, 8));
    uint8_t *w = pg + 32 + (i / 16u) * 4u;
    wr32(w, rd32(w) & ~(1u << ((i % 16u) * 2u)));   /* 11 (empty) -> 10 (written) */
}

/* NVS reads FULL and FREEING pages as it reads the ACTIVE one; CORRUPT (0xFFFFFFF0), INVALID (0)
 * and uninitialised pages it does not read. The state word is outside the header CRC. */
static void test_page_states(void)
{
    static const struct {
        uint32_t state;
        bool read;
    } k[] = {{0xFFFFFFFEu, true}, {0xFFFFFFFCu, true}, {0xFFFFFFF8u, true},
             {0xFFFFFFF0u, false}, {0x00000000u, false}, {0xFFFFFFFFu, false}};
    for (size_t n = 0; n < sizeof(k) / sizeof(k[0]); n++) {
        for (int enc = 0; enc < 2; enc++) {
            gw_nvsraw_scan_t s;
            load(enc ? k_enc : k_plain);
            wr32(page, k[n].state);
            gw_nvsraw_kind_t v = scan(page, &s);
            TEST_ASSERT_EQUAL_UINT32(k[n].read ? 1 : 0, s.pages);
            TEST_ASSERT_EQUAL(k[n].read ? (enc ? GW_NVSRAW_ENCRYPTED : GW_NVSRAW_PLAIN)
                                        : GW_NVSRAW_EMPTY, v);
        }
        /* the bdSeq is read from a full or freeing page too (a page that filled up) */
        gw_nvsraw_u8_t f;
        gw_nvsraw_u8_init(&f, "spb", "bdseq");
        load(k_plain);
        wr32(page, k[n].state);
        gw_nvsraw_u8_ns(&f, page);
        gw_nvsraw_u8_item(&f, page);
        TEST_ASSERT_EQUAL(k[n].read, f.found);
    }
    /* a migration interrupted on a device whose NVS spans pages: one full page still plain, the
     * active one already encrypted -> mixed (the plain entries are the newer ones, #94) */
    static uint8_t full[GW_NVSRAW_PAGE];
    memcpy(full, load(k_plain), sizeof(full));
    wr32(full, 0xFFFFFFFCu);
    gw_nvsraw_scan_t s;
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, full);
    gw_nvsraw_scan_page(&s, load(k_enc));
    TEST_ASSERT_EQUAL_UINT32(2, s.pages);
    TEST_ASSERT_EQUAL(GW_NVSRAW_MIXED, gw_nvsraw_verdict(&s));
}

/* Decision (issue #97): a page counts as plain while its valid plain items are at least as many
 * as its written entries that fail the CRC (`plain_ok >= bad`), and that stays. A valid plain
 * CRC on an XTS-encrypted entry is a 2^-32 chance, but a page read as plain next to encrypted
 * ones is MIXED and migrated, erasing the encrypted entries: the rule must not let a few chance
 * matches win. A real unencrypted NVS does not have more bad entries than good ones (NVS marks
 * an entry written only after writing it); if bit rot gets there, the page reads as encrypted,
 * the boot opens the encrypted NVS and the old entries are lost, as they would be with any
 * reading of so damaged a partition. Note that a corrupt item header counts its data entries as
 * bad too (its span cannot be trusted): three corrupt strings or blobs on a page of eight items
 * are enough (README 6.2.1). */
static void test_more_bad_than_plain(void)
{
    gw_nvsraw_scan_t s;
    /* page 0 of k_plain: namespaces at 0, 2, 7; bdseq at 1; strings at 3 (+4) and 5 (+6); the
     * lkg_net blob data at 8 (+9) and its index at 10: 8 items in 11 written entries */
    /* the commission and mqtt_pw headers corrupt: 6 valid items, 4 bad entries: plain */
    load(k_plain);
    page[64 + 32 * 3 + 9] ^= 0x01;
    page[64 + 32 * 5 + 9] ^= 0x01;
    TEST_ASSERT_EQUAL(GW_NVSRAW_PLAIN, scan(page, &s));
    TEST_ASSERT_EQUAL_UINT32(6, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(4, s.bad);
    /* and the bdseq: 5 and 5, a tie is still plain */
    page[64 + 32 * 1 + 9] ^= 0x01;
    TEST_ASSERT_EQUAL(GW_NVSRAW_PLAIN, scan(page, &s));
    TEST_ASSERT_EQUAL_UINT32(5, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(5, s.bad);
    /* the two strings and the blob data instead: 5 valid items, 6 bad entries: read as
     * encrypted, and so not migrated (the boot opens the encrypted NVS) */
    load(k_plain);
    page[64 + 32 * 3 + 9] ^= 0x01;
    page[64 + 32 * 5 + 9] ^= 0x01;
    page[64 + 32 * 8 + 9] ^= 0x01;
    TEST_ASSERT_EQUAL(GW_NVSRAW_ENCRYPTED, scan(page, &s));
    TEST_ASSERT_EQUAL_UINT32(5, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(6, s.bad);
    TEST_ASSERT_EQUAL_UINT32(0, s.plain_pages);
    TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, decide(GW_NVSRAW_ENCRYPTED, GW_NVSMIG_COPY_NONE,
                                                       false, true, true));
    /* one such page next to a sound plain page: mixed, migrated (the sound page's entries are
     * kept; the damaged ones were unreadable anyway) */
    static uint8_t damaged[GW_NVSRAW_PAGE];
    memcpy(damaged, page, sizeof(damaged));
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, damaged);
    gw_nvsraw_scan_page(&s, load(k_plain));
    TEST_ASSERT_EQUAL(GW_NVSRAW_MIXED, gw_nvsraw_verdict(&s));
    TEST_ASSERT_EQUAL(GW_NVSMIG_MIGRATE, decide(GW_NVSRAW_MIXED, GW_NVSMIG_COPY_NONE, false, true,
                                                true));
}

/* The span of a valid item is trusted only while it ends inside the page: past the last entry
 * (126) or 0 it counts as 1, so the entries after it are still read. */
static void test_span_trimmed(void)
{
    static uint8_t pg[GW_NVSRAW_PAGE];
    const uint8_t *bdseq = k_plain + 64 + 32;   /* a u8 item, span 1 */
    gw_nvsraw_scan_t s;
    /* ends exactly at the last entry: its data entry (125, not an item) is not counted bad */
    memset(pg, 0xFF, sizeof(pg));
    memcpy(pg, k_plain, 32);                    /* active page header */
    put_entry(pg, 124, bdseq, 2);
    wr32(pg + 32 + 7 * 4, rd32(pg + 32 + 7 * 4) & ~(1u << 26));   /* 125 written (data) */
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, pg);
    TEST_ASSERT_EQUAL_UINT32(2, s.written);
    TEST_ASSERT_EQUAL_UINT32(1, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(0, s.bad);
    /* past the end: span 3 at 124 trimmed to 1, the entry at 125 read on its own (and bad) */
    put_entry(pg, 124, bdseq, 3);
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, pg);
    TEST_ASSERT_EQUAL_UINT32(2, s.written);
    TEST_ASSERT_EQUAL_UINT32(1, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(1, s.bad);
    /* the last entry with span 2, and span 0 at the first entry: 1 each, the next one read */
    memset(pg, 0xFF, sizeof(pg));
    memcpy(pg, k_plain, 32);
    put_entry(pg, 0, bdseq, 0);
    put_entry(pg, 1, bdseq, 1);
    put_entry(pg, 125, bdseq, 2);
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_scan_page(&s, pg);
    TEST_ASSERT_EQUAL_UINT32(3, s.written);
    TEST_ASSERT_EQUAL_UINT32(3, s.plain_ok);
    TEST_ASSERT_EQUAL_UINT32(0, s.bad);
    TEST_ASSERT_EQUAL(GW_NVSRAW_PLAIN, gw_nvsraw_verdict(&s));
    /* a huge span (255) at 2 is trimmed too: the raw bdSeq after it is still found */
    memset(pg, 0xFF, sizeof(pg));
    memcpy(pg, k_plain, 32);
    put_entry(pg, 0, k_plain + 64, 1);          /* namespace spb = 1 */
    put_entry(pg, 2, bdseq, 255);
    static uint8_t later[32];
    memcpy(later, bdseq, 32);
    later[24] = 77;
    put_entry(pg, 3, later, 1);
    gw_nvsraw_u8_t f;
    gw_nvsraw_u8_init(&f, "spb", "bdseq");
    gw_nvsraw_u8_ns(&f, pg);
    gw_nvsraw_u8_item(&f, pg);
    TEST_ASSERT_TRUE(f.found);
    TEST_ASSERT_EQUAL_UINT8(77, f.value);
    TEST_ASSERT_EQUAL_UINT32(3, f.entry);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_esp_rom);
    RUN_TEST(test_plain_image);
    RUN_TEST(test_encrypted_image);
    RUN_TEST(test_erased_or_empty);
    RUN_TEST(test_one_corrupt_entry_keeps_plain);
    RUN_TEST(test_several_pages_accumulate);
    RUN_TEST(test_mixed_pages);
    RUN_TEST(test_raw_u8_bdseq);
    RUN_TEST(test_copy_round_trip);
    RUN_TEST(test_decide);
    RUN_TEST(test_decide_with_copy);
    RUN_TEST(test_may_erase);
    RUN_TEST(test_bdseq_later);
    RUN_TEST(test_decide_copy_out_of_reach);
    RUN_TEST(test_bdseq_reservation);
    RUN_TEST(test_bdseq_reservation_chain);
    RUN_TEST(test_copy_load);
    RUN_TEST(test_page_states);
    RUN_TEST(test_more_bad_than_plain);
    RUN_TEST(test_span_trimmed);
    return UNITY_END();
}
