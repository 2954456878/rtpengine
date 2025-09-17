#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>


struct rtpengine_output {
	struct {
		uint8_t extmap_filter[32];
		unsigned int num_extmap_filter;

		uint8_t extmap_mid;
		uint8_t extmap_mid_len;
		char extmap_mid_str[32];
	} output;
};

struct sk_buff {
	unsigned int len;
};

struct rtp_header {
	unsigned char v_p_x_cc;
	unsigned char _pad[3];
};

struct rtp_exthdr {
	uint16_t undefined;
	uint16_t length;
} __attribute__ ((packed));

struct rtp_parsed {
	struct rtp_header *rtp_header;
	unsigned char *payload;
	size_t payload_len;
	struct rtp_exthdr *ext_hdr;
	unsigned char *extension;
	size_t extension_len;
};

static void skb_trim(struct sk_buff *s, unsigned int len) {
	s->len = len;
}
static void skb_put(struct sk_buff *s, unsigned int len) {
	s->len += len;
}

#include "../kernel-module/extmap_filter.inc.c"

static void pkt(unsigned char *d, struct sk_buff *skb, struct rtp_parsed *r,
		uint8_t hdr_val,
		size_t ext_hdr_len, const unsigned char *ext_hdr,
		size_t extensions_len, const unsigned char *extensions)
{
	r->rtp_header = (struct rtp_header *) d;
	memset(r->rtp_header, 0, sizeof(*r->rtp_header));
	*d = hdr_val;
	d += sizeof(struct rtp_header);

	if (ext_hdr_len == 0) {
		assert(extensions_len == 0);
		r->ext_hdr = NULL;
	}
	else if (ext_hdr_len == 2) {
		r->ext_hdr = (struct rtp_exthdr *) d;
		*d++ = ext_hdr[0];
		*d++ = ext_hdr[1];
		size_t padded = (extensions_len + 3L) & ~3L;
		// verify math
		assert((padded & 0x3) == 0);
		assert(padded >= extensions_len);
		size_t padding = padded - extensions_len;
		assert(padding < 4);
		size_t blocks = padded / 4L;
		assert(blocks <= 0xffff);
		*d++ = blocks >> 8;
		*d++ = blocks & 0xff;
		r->extension = d;
		memcpy(d, extensions, extensions_len);
		d += extensions_len;
		memset(d, 0, padding);
		d += padding;
		r->extension_len = padded;
	}
	else
		abort();

	// fixed dummy payload
	r->payload = d;
	for (unsigned i = 0; i < 128; i++)
		*d++ = i + 64;
	r->payload_len = 128;

	skb->len = d - (unsigned char *) r->rtp_header;
}

static void dump(uint8_t *d, size_t len) {
	for (size_t i = 0; i < len; i++)
		printf("%02x ", d[i]);
	printf("\n");
}

static void tester(
		unsigned int line,
		uint8_t rtp_hdr_val_in, size_t ext_hdr_in_len, const unsigned char *ext_hdr_in,
		size_t extensions_in_len, const unsigned char *extensions_in,
		unsigned int filter_len, const uint8_t *filter,
		uint8_t rtp_hdr_val_exp, size_t ext_hdr_exp_len, const unsigned char *ext_hdr_exp,
		size_t extensions_exp_len, const unsigned char *extensions_exp)
{
	printf("test @ line %u\n", line);

	// build packets
	unsigned char in [sizeof(struct rtp_header) + ext_hdr_in_len  + 2 + extensions_in_len  + 3 + 128];
	unsigned char exp[sizeof(struct rtp_header) + ext_hdr_exp_len + 2 + extensions_exp_len + 3 + 128];
	struct sk_buff is;
	struct sk_buff es;
	struct rtp_parsed ip;
	struct rtp_parsed ep;

	pkt(in,  &is, &ip,  rtp_hdr_val_in,  ext_hdr_in_len,  ext_hdr_in,  extensions_in_len,  extensions_in);
	pkt(exp, &es, &ep,  rtp_hdr_val_exp, ext_hdr_exp_len, ext_hdr_exp, extensions_exp_len, extensions_exp);

	struct rtpengine_output o = {0};
	assert(filter_len <= sizeof(o.output.extmap_filter));
	o.output.num_extmap_filter = filter_len;
	memcpy(o.output.extmap_filter, filter, filter_len);

	apply_extmap_filter(&is, &o, &ip);

	if (is.len != es.len) {
		printf("%u != %u\n", is.len, es.len);
		assert(0);
	}
	if (memcmp(in, exp, is.len)) {
		dump(in, is.len);
		dump(exp, is.len);
		assert(0);
	}
	assert(ip.payload_len == ep.payload_len);
	assert(memcmp(ip.payload, ep.payload, ip.payload_len) == 0);

	printf("ok\n");
}

#define TEST( \
		rtp_hdr_val_in, ext_hdr_in, extensions_in, \
		filter, \
		rtp_hdr_val_exp, ext_hdr_exp, extensions_exp \
	) \
	tester( \
			__LINE__, \
			rtp_hdr_val_in, \
			sizeof(ext_hdr_in) - 1, (unsigned char *) ext_hdr_in, \
			sizeof(extensions_in) - 1, (unsigned char *) extensions_in, \
			sizeof(filter) - 1, (uint8_t *) filter, \
			rtp_hdr_val_exp, \
			sizeof(ext_hdr_exp) - 1, (unsigned char *) ext_hdr_exp, \
			sizeof(extensions_exp) - 1, (unsigned char *) extensions_exp \
	);

int main(void) {
	// no extensions, no filter
	TEST(
			0x80, "", "",
			"",
			0x80, "", ""
	);

	// no extensions, filter
	TEST(
			0x80, "", "",
			"\x01\x02\x03\x04",
			0x80, "", ""
	);


	// one-byte extension, empty filter (not allowed)
	TEST(
			0x90, "\xbe\xde", "\x12" "foo",
			"",
			0x80, "", ""
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x",
			"",
			0x80, "", ""
	);


	// multiple one-byte extensions, empty filter (not allowed)
	TEST(
			0x90, "\xbe\xde", "\x12" "foo"   "\x22" "bar"   "\x32" "yax"   "\x42" "wuz",
			"",
			0x80, "", ""
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\x20" "y" "\x30" "z" "\x40" "p",
			"",
			0x80, "", ""
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\0\0"   "\x20" "y" "\0\0"   "\x30" "z" "\0\0"   "\x40" "p",
			"",
			0x80, "", ""
	);


	// multiple one-byte extensions, allow first
	TEST(
			0x90, "\xbe\xde", "\x12" "foo"   "\x22" "bar"   "\x32" "yax"   "\x42" "wuz",
			"\x01",
			0x90, "\xbe\xde", "\x12" "foo"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\x20" "y" "\x30" "z" "\x40" "p",
			"\x01",
			0x90, "\xbe\xde", "\x10" "x"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\0\0"   "\x20" "y" "\0\0"   "\x30" "z" "\0\0"   "\x40" "p",
			"\x01",
			0x90, "\xbe\xde", "\x10" "x"
	);


	// multiple one-byte extensions, allow second
	TEST(
			0x90, "\xbe\xde", "\x12" "foo"   "\x22" "bar"   "\x32" "yax"   "\x42" "wuz",
			"\x02",
			0x90, "\xbe\xde", "\x22" "bar"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\x20" "y" "\x30" "z" "\x40" "p",
			"\x02",
			0x90, "\xbe\xde", "\x20" "y"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\0\0"   "\x20" "y" "\0\0"   "\x30" "z" "\0\0"   "\x40" "p",
			"\x02",
			0x90, "\xbe\xde", "\x20" "y"
	);


	// multiple one-byte extensions, allow last
	TEST(
			0x90, "\xbe\xde", "\x12" "foo"   "\x22" "bar"   "\x32" "yax"   "\x42" "wuz",
			"\x04",
			0x90, "\xbe\xde", "\x42" "wuz"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\x20" "y" "\x30" "z" "\x40" "p",
			"\x04",
			0x90, "\xbe\xde", "\x40" "p"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\0\0"   "\x20" "y" "\0\0"   "\x30" "z" "\0\0"   "\x40" "p",
			"\x04",
			0x90, "\xbe\xde", "\x40" "p"
	);


	// multiple one-byte extensions, allow first and third
	TEST(
			0x90, "\xbe\xde", "\x12" "foo"   "\x22" "bar"   "\x32" "yax"   "\x42" "wuz",
			"\x01\x03",
			0x90, "\xbe\xde", "\x12" "foo"   "\x32" "yax"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\x20" "y" "\x30" "z" "\x40" "p",
			"\x01\x03",
			0x90, "\xbe\xde", "\x10" "x" "\x30" "z" 
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\0\0"   "\x20" "y" "\0\0"   "\x30" "z" "\0\0"   "\x40" "p",
			"\x01\x03",
			0x90, "\xbe\xde", "\x10" "x" "\x30" "z"
	);


	// multiple one-byte extensions, allow second and last
	TEST(
			0x90, "\xbe\xde", "\x12" "foo"   "\x22" "bar"   "\x32" "yax"   "\x42" "wuz",
			"\x02\x04",
			0x90, "\xbe\xde", "\x22" "bar"   "\x42" "wuz"
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\x20" "y" "\x30" "z" "\x40" "p",
			"\x02\x04",
			0x90, "\xbe\xde", "\x20" "y" "\x40" "p" 
	);
	TEST(
			0x90, "\xbe\xde", "\x10" "x" "\0\0"   "\x20" "y" "\0\0"   "\x30" "z" "\0\0"   "\x40" "p",
			"\x02\x04",
			0x90, "\xbe\xde", "\x20" "y" "\x40" "p"
	);


	// random padding, allow multiple
	TEST(
			0x90, "\xbe\xde",
				"\x10" "a"      "\x20" "b" "\0"     "\x30" "c" "\0\0"      "\x40" "d" "\0\0\0"
				"\x51" "ee"     "\x61" "ff" "\0"    "\x71" "gg" "\0\0"     "\x81" "hh" "\0\0\0"
				"\x92" "kkk"    "\xa2" "lll" "\0"   "\xb2" "mmm" "\0\0"    "\xc2" "nnn" "\0\0\0"
				"\xd3" "oooo",
			"\x01\x04\x07\x0a\x0c\x0d",
			0x90, "\xbe\xde", "\x10" "a"   "\x40" "d"   "\x71" "gg"   "\xa2" "lll"   "\xc2" "nnn" "\xd3" "oooo"  
	);

	TEST(
			0x90, "\xbe\xde",
				"\x10" "a"      "\x20" "b" "\0"     "\x30" "c" "\0\0"      "\x40" "d" "\0\0\0"
				"\x51" "ee"     "\x61" "ff" "\0"    "\x71" "gg" "\0\0"     "\x81" "hh" "\0\0\0"
				"\x92" "kkk"    "\xa2" "lll" "\0"   "\xb2" "mmm" "\0\0"    "\xc2" "nnn" "\0\0\0"
				"\xd5" "oooooo",
			"\x01\x04\x07\x0a\x0c\x0d",
			0x90, "\xbe\xde", "\x10" "a"   "\x40" "d"   "\x71" "gg"   "\xa2" "lll"   "\xc2" "nnn" "\xd5" "oooooo"  
	);


	// two-byte extension, empty filter (not allowed)
	TEST(
			0x90, "\x01\x00", "\x01\x03" "foo",
			"",
			0x80, "", ""
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x",
			"",
			0x80, "", ""
	);


	// multiple two-byte extensions, empty filter (not allowed)
	TEST(
			0x90, "\x01\x00", "\x01\x03" "foo"   "\x02\x03" "bar"   "\x03\x03" "yax"   "\x04\x03" "wuz",
			"",
			0x80, "", ""
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\x02\x01" "y" "\x03\x01" "z" "\x40" "p",
			"",
			0x80, "", ""
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\0\0"   "\x02\x01" "y" "\0\0"   "\x03\x01" "z" "\0\0"   "\x40" "p",
			"",
			0x80, "", ""
	);


	// multiple two-byte extensions, allow first
	TEST(
			0x90, "\x01\x00", "\x01\x03" "foo"   "\x02\x03" "bar"   "\x03\x03" "yax"   "\x04\x03" "wuz",
			"\x01",
			0x90, "\x01\x00", "\x01\x03" "foo"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\x02\x01" "y" "\x03\x01" "z" "\x40" "p",
			"\x01",
			0x90, "\x01\x00", "\x01\x01" "x"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\0\0"   "\x02\x01" "y" "\0\0"   "\x03\x01" "z" "\0\0"   "\x40" "p",
			"\x01",
			0x90, "\x01\x00", "\x01\x01" "x"
	);


	// multiple two-byte extensions, allow second
	TEST(
			0x90, "\x01\x00", "\x01\x03" "foo"   "\x02\x03" "bar"   "\x03\x03" "yax"   "\x04\x03" "wuz",
			"\x02",
			0x90, "\x01\x00", "\x02\x03" "bar"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\x02\x01" "y" "\x03\x01" "z" "\x40" "p",
			"\x02",
			0x90, "\x01\x00", "\x02\x01" "y"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\0\0"   "\x02\x01" "y" "\0\0"   "\x03\x01" "z" "\0\0"   "\x40" "p",
			"\x02",
			0x90, "\x01\x00", "\x02\x01" "y"
	);


	// multiple two-byte extensions, allow last
	TEST(
			0x90, "\x01\x00", "\x01\x03" "foo"   "\x02\x03" "bar"   "\x03\x03" "yax"   "\x04\x03" "wuz",
			"\x04",
			0x90, "\x01\x00", "\x04\x03" "wuz"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\x02\x01" "y" "\x03\x01" "z" "\x04\x01" "p",
			"\x04",
			0x90, "\x01\x00", "\x04\x01" "p"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\0\0"   "\x02\x01" "y" "\0\0"   "\x03\x01" "z" "\0\0"   "\x04\x01" "p",
			"\x04",
			0x90, "\x01\x00", "\x04\x01" "p"
	);


	// multiple two-byte extensions, allow first and third
	TEST(
			0x90, "\x01\x00", "\x01\x03" "foo"   "\x02\x03" "bar"   "\x03\x03" "yax"   "\x04\x03" "wuz",
			"\x01\x03",
			0x90, "\x01\x00", "\x01\x03" "foo"   "\x03\x03" "yax"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\x02\x01" "y" "\x03\x01" "z" "\x04\x01" "p",
			"\x01\x03",
			0x90, "\x01\x00", "\x01\x01" "x" "\x03\x01" "z" 
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\0\0"   "\x02\x01" "y" "\0\0"   "\x03\x01" "z" "\0\0"   "\x04\x01" "p",
			"\x01\x03",
			0x90, "\x01\x00", "\x01\x01" "x" "\x03\x01" "z"
	);


	// multiple two-byte extensions, allow second and last
	TEST(
			0x90, "\x01\x00", "\x01\x03" "foo"   "\x02\x03" "bar"   "\x03\x03" "yax"   "\x04\x03" "wuz",
			"\x02\x04",
			0x90, "\x01\x00", "\x02\x03" "bar"   "\x04\x03" "wuz"
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\x02\x01" "y" "\x03\x01" "z" "\x04\x01" "p",
			"\x02\x04",
			0x90, "\x01\x00", "\x02\x01" "y" "\x04\x01" "p" 
	);
	TEST(
			0x90, "\x01\x00", "\x01\x01" "x" "\0\0"   "\x02\x01" "y" "\0\0"   "\x03\x01" "z" "\0\0"   "\x04\x01" "p",
			"\x02\x04",
			0x90, "\x01\x00", "\x02\x01" "y" "\x04\x01" "p"
	);


	// random padding, allow multiple
	TEST(
			0x90, "\x01\x00",
				"\x01\x01" "a"      "\x02\x01" "b" "\0"     "\x03\x01" "c" "\0\0"      "\x04\x01" "d" "\0\0\0"
				"\x05\x02" "ee"     "\x06\x02" "ff" "\0"    "\x07\x02" "gg" "\0\0"     "\x08\x02" "hh" "\0\0\0"
				"\x09\x03" "kkk"    "\x0a\x03" "lll" "\0"   "\x0b\x03" "mmm" "\0\0"    "\x0c\x03" "nnn" "\0\0\0"
				"\x0d\x04" "oooo",
			"\x01\x04\x07\x0a\x0c\x0d",
			0x90, "\x01\x00", "\x01\x01" "a"   "\x04\x01" "d"   "\x07\x02" "gg"   "\x0a\x03" "lll"   "\x0c\x03" "nnn" "\x0d\04" "oooo"  
	);

	TEST(
			0x90, "\x01\x00",
				"\x01\x01" "a"      "\x02\x01" "b" "\0"     "\x03\x01" "c" "\0\0"      "\x04\x01" "d" "\0\0\0"
				"\x05\x02" "ee"     "\x06\x02" "ff" "\0"    "\x07\x02" "gg" "\0\0"     "\x08\x02" "hh" "\0\0\0"
				"\x09\x03" "kkk"    "\x0a\x03" "lll" "\0"   "\x0b\x03" "mmm" "\0\0"    "\x0c\x03" "nnn" "\0\0\0"
				"\x0d\x06" "oooooo",
			"\x01\x04\x07\x0a\x0c\x0d",
			0x90, "\x01\x00", "\x01\x01" "a"   "\x04\x01" "d"   "\x07\x02" "gg"   "\x0a\x03" "lll"   "\x0c\x03" "nnn" "\x0d\x06" "oooooo"  
	);


	// higher IDs and longer values
	TEST(
			0x90, "\x01\x00",
				"\x31\x01" "a"      "\x32\x21" "bxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" "\0"     "\x33\x01" "c" "\0\0"      "\x34\x21" "dxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" "\0\0\0"
				"\x35\x02" "ee"     "\x36\x22" "ffxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" "\0"    "\x37\x02" "gg" "\0\0"     "\x38\x22" "hhxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" "\0\0\0"
				"\x39\x03" "kkk"    "\x3a\x23" "lllxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" "\0"   "\x3b\x03" "mmm" "\0\0"    "\x3c\x23" "nnnxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" "\0\0\0"
				"\x3d\x04" "oooo",
			"\x31\x34\x37\x3a\x3c\x3d",
			0x90, "\x01\x00", "\x31\x01" "a"   "\x34\x21" "dxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"   "\x37\x02" "gg"   "\x3a\x23" "lllxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"   "\x3c\x23" "nnnxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" "\x3d\04" "oooo"  
	);


	return 0;
}
