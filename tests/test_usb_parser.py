"""Run the actual USB reassembly code with process-space buffer/callback stubs."""
from pathlib import Path
import os
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class USBParserTests(unittest.TestCase):
    @unittest.skipUnless(platform.system() == 'Linux', 'parser harness requires Linux UAPI/endian headers')
    def test_fragmentation_coalescing_and_resync(self):
        source = (ROOT / 'driver/vcam_usb.c').read_text()
        parser = source[source.index('enum vcam_rx_state'):source.index('static void vcam_usb_read_complete')]
        harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <endian.h>
#include "vcam_uapi.h"
typedef uint8_t u8;
typedef uint32_t u32;
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define le16_to_cpu(x) le16toh(x)
#define le32_to_cpu(x) le32toh(x)
#define dev_warn_ratelimited(...) ((void)0)
struct vcam_dev { u32 width, height, fourcc; };
static unsigned int submitted;
static unsigned char expected[256];
static int vcam_submit_frame(struct vcam_dev *dev, const void *data, size_t len)
{
    assert(len == sizeof(expected));
    assert(!memcmp(data, expected, len));
    submitted++;
    return 0;
}
''' + parser + r'''
int main(void)
{
    struct vcam_dev dev = { 16, 16, 0x59455247 };
    unsigned char guarded[258], record[288], stream[700];
    struct vcam_usb vu = { .vcam = &dev, .frame_buf = guarded + 1, .frame_size = 256 };
    struct vcam_frame_hdr hdr = {
        .magic = htole32(VCAM_FRAME_MAGIC), .width = htole16(16),
        .height = htole16(16), .fourcc = htole32(0x59455247),
        .payload_len = htole32(256)
    };
    _Static_assert(sizeof(hdr) == 32, "wire header changed");
    _Static_assert(sizeof(struct vcam_usb_info) == 16, "GET_INFO changed");
    memset(guarded, 0xa5, sizeof(guarded));
    for (int i = 0; i < 256; i++) expected[i] = i;
    memcpy(record, &hdr, 32);
    memcpy(record + 32, expected, 256);
    /* Every possible uniform chunk size, including one-byte headers/payloads. */
    for (unsigned step = 1; step <= sizeof(record); step++) {
        vu.state = VCAM_RX_HDR; vu.hdr_have = 0; submitted = 0;
        for (unsigned off = 0; off < sizeof(record); off += step) {
            unsigned len = min_t(unsigned, step, sizeof(record) - off);
            vcam_usb_parse(&vu, record + off, len);
        }
        assert(submitted == 1);
        assert(guarded[0] == 0xa5 && guarded[257] == 0xa5);
    }
    /* Junk, impossible payload length, then two valid records in one URB. */
    memset(stream, 0x55, 7);
    hdr.payload_len = htole32(0xffffffff);
    memcpy(stream + 7, &hdr, 32);
    memcpy(stream + 39, record, 288);
    memcpy(stream + 327, record, 288);
    vu.state = VCAM_RX_HDR; vu.hdr_have = 0; submitted = 0;
    vcam_usb_parse(&vu, stream, 615);
    assert(submitted == 2);
    /* A zero-size malformed header must also make progress. */
    hdr.payload_len = 0;
    memcpy(stream, &hdr, 32); memcpy(stream + 32, record, 288);
    submitted = 0;
    vcam_usb_parse(&vu, stream, 320);
    assert(submitted == 1);
    puts("USB parser: PASS");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'parser.c').write_text(harness)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                            '-Wno-unused-parameter', '-I', str(ROOT / 'driver'),
                            str(path / 'parser.c'), '-o', str(path / 'parser')], check=True)
            subprocess.run([str(path / 'parser')], check=True, timeout=10)
