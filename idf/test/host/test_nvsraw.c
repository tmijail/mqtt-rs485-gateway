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
    size_t size = gw_nvsmig_seal(buf, n);
    uint32_t count = 0;
    TEST_ASSERT_TRUE(gw_nvsmig_check(buf, size, &count));
    TEST_ASSERT_EQUAL_UINT32(3, count);
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
    size = gw_nvsmig_seal(e, gw_nvsmig_begin(e, sizeof(e)));
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

static void test_decide(void)
{
    const gw_nvsraw_kind_t kinds[] = {GW_NVSRAW_EMPTY, GW_NVSRAW_PLAIN, GW_NVSRAW_ENCRYPTED,
                                      GW_NVSRAW_MIXED};
    /* a valid copy of an interrupted migration is always finished, whatever the pages hold */
    for (unsigned k = 0; k < 4; k++) {
        for (int t = 0; t < 2; t++) {
            TEST_ASSERT_EQUAL(GW_NVSMIG_RESUME,
                              gw_nvsmig_decide(kinds[k], GW_NVSMIG_COPY_VALID, t, true));
        }
    }
    for (int c = 0; c < 2; c++) {   /* no copy, or a damaged one (discarded) */
        gw_nvsmig_copy_t copy = c ? GW_NVSMIG_COPY_BAD : GW_NVSMIG_COPY_NONE;
        /* unencrypted (or mixed: the plain entries are the newer ones) on a confirmed image */
        TEST_ASSERT_EQUAL(GW_NVSMIG_MIGRATE, gw_nvsmig_decide(GW_NVSRAW_PLAIN, copy, false, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_MIGRATE, gw_nvsmig_decide(GW_NVSRAW_MIXED, copy, false, true));
        /* on trial: a rollback must find what the previous image can read */
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(GW_NVSRAW_PLAIN, copy, true, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(GW_NVSRAW_MIXED, copy, true, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(GW_NVSRAW_EMPTY, copy, true, true));
        /* nowhere to keep the copy: nothing is erased */
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(GW_NVSRAW_PLAIN, copy, false, false));
        TEST_ASSERT_EQUAL(GW_NVSMIG_STAY_PLAIN, gw_nvsmig_decide(GW_NVSRAW_MIXED, copy, false, false));
        /* already encrypted (also on trial), or empty on a confirmed image */
        for (int t = 0; t < 2; t++) {
            for (int f = 0; f < 2; f++) {
                TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED,
                                  gw_nvsmig_decide(GW_NVSRAW_ENCRYPTED, copy, t, f));
            }
        }
        TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, gw_nvsmig_decide(GW_NVSRAW_EMPTY, copy, false, true));
        TEST_ASSERT_EQUAL(GW_NVSMIG_OPEN_ENCRYPTED, gw_nvsmig_decide(GW_NVSRAW_EMPTY, copy, false, false));
    }
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
    return UNITY_END();
}
