/*
 * Issue #226: the NVS partition and the copy of its entries (issue #94), as the boot reads them
 * from the flash and from LittleFS. The raw pages of an NVS partition (gw_nvsraw_scan_page, the
 * u8 item lookup and the erase marker of a migration), and the copy file of a migration
 * (gw_nvsmig_load, gw_nvsmig_check, gw_nvsmig_next, gw_nvsmig_find, gw_nvsmig_orphan_in_copy).
 *
 * Input: a selector byte (bit 0: raw pages, else a copy file; bit 1: correct the CRCs, so that
 * the input gets past them; bit 2: the CRC of a copy of version 1), then the data.
 */
#include "fuzz.h"
#include "gw_nvsraw.h"

/* The NVS item type of a u8 (nvs_type_t NVS_TYPE_U8). */
#define NVS_TYPE_U8 0x01u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The CRCs of an NVS page: the header (bytes 4-27, at 28) and each of the 126 entries of 32 bytes
 * after the header and the state bitmap (Item::calculateCrc32, at byte 4 of the entry). */
static void fix_page(uint8_t *page)
{
    put32(page + 28, gw_nvsraw_crc32(0xFFFFFFFFu, page + 4, 24));
    for (uint32_t i = 0; i < 126u; i++) {
        uint8_t *e = page + 64u + 32u * i;
        uint32_t c = gw_nvsraw_crc32(0xFFFFFFFFu, e, 4);
        c = gw_nvsraw_crc32(c, e + 8, 16);
        c = gw_nvsraw_crc32(c, e + 24, 8);
        put32(e + 4, c);
    }
}

static void raw_pages(const uint8_t *data, size_t len, bool fix_crc)
{
    gw_nvsraw_scan_t s;
    gw_nvsraw_scan_init(&s);
    gw_nvsraw_u8_t u;
    gw_nvsraw_u8_init(&u, "gw", "boot_count");
    gw_nvsraw_u8_t mark;
    gw_nvsraw_u8_init(&mark, "gw_mig", GW_NVSMIG_ERASE_KEY);
    for (size_t off = 0; off < len && off < 4u * GW_NVSRAW_PAGE; off += GW_NVSRAW_PAGE) {
        /* a page of exactly GW_NVSRAW_PAGE bytes; a short last page is filled with 0xFF (erased) */
        uint8_t *page = malloc(GW_NVSRAW_PAGE);
        if (page == NULL) {
            abort();
        }
        size_t n = len - off < GW_NVSRAW_PAGE ? len - off : GW_NVSRAW_PAGE;
        memset(page, 0xFF, GW_NVSRAW_PAGE);
        memcpy(page, data + off, n);
        if (fix_crc) {
            fix_page(page);
        }
        gw_nvsraw_scan_page(&s, page);
        gw_nvsraw_u8_ns(&u, page);
        gw_nvsraw_u8_item(&u, page);
        gw_nvsraw_u8_ns(&mark, page);
        gw_nvsraw_u8_item(&mark, page);
        (void)gw_nvsraw_has_erase_marker(page);
        free(page);
    }
    gw_nvsraw_kind_t k = gw_nvsraw_verdict(&s);
    FZ_CHECK(k <= GW_NVSRAW_MIXED);
}

static void copy_file(const uint8_t *data, size_t len, bool fix_crc, bool v1)
{
    /* the file, read whole into a buffer with 4 bytes more (a version 1 copy grows) */
    size_t cap = len + 4;
    uint8_t *buf = malloc(cap);
    if (buf == NULL) {
        abort();
    }
    memcpy(buf, data, len);
    size_t hdr = v1 ? 16u : GW_NVSMIG_HDR;   /* the header of version 1 has 16 bytes */
    if (fix_crc && len >= hdr) {
        /* the CRC of the body, at offset 12 of the header (gw_nvsmig_seal) */
        put32(buf + 12, gw_nvsraw_crc32(0, buf + hdr, len - hdr));
    }
    uint32_t count = 0;
    bool checked = gw_nvsmig_check(buf, len, &count);
    if (checked) {
        (void)gw_nvsmig_crc(buf);
        (void)gw_nvsmig_skipped(buf);
    }
    /* the records, also of a copy that is not sealed */
    size_t off = 0;
    gw_nvsmig_rec_t r;
    uint32_t seen = 0;
    while (gw_nvsmig_next(buf, len, &off, &r) && seen < 100000) {
        FZ_CHECK(r.ns != NULL && r.key != NULL);
        FZ_CHECK(r.ns >= (const char *)buf && r.ns < (const char *)buf + len);
        FZ_CHECK(strlen(r.ns) < GW_NVSRAW_NAME && strlen(r.key) < GW_NVSRAW_NAME);
        FZ_CHECK(r.len == 0 || (r.data >= buf && r.data + r.len <= buf + len));
        (void)gw_nvsmig_orphan_in_copy(buf, len, "", r.key, r.type);
        seen++;
    }
    if (checked) {
        FZ_CHECK(seen == count);
    }
    (void)gw_nvsmig_find(buf, len, "gw", "boot_count", &r);
    (void)gw_nvsmig_orphan_in_copy(buf, len, "", "boot_count", NVS_TYPE_U8);

    size_t out_size = 0;
    bool legacy = false;
    gw_nvsmig_copy_t c = gw_nvsmig_load(buf, len, cap, &out_size, &legacy, &count);
    if (c == GW_NVSMIG_COPY_VALID) {
        FZ_CHECK(out_size <= cap);
        FZ_CHECK(gw_nvsmig_check(buf, out_size, &count));
    }
    free(buf);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t sel = fz_u8(&f);
    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    if ((sel & 1u) != 0) {
        raw_pages(rest, len, (sel & 2u) != 0);
    } else {
        copy_file(rest, len, (sel & 2u) != 0, (sel & 4u) != 0);
    }
    return 0;
}
