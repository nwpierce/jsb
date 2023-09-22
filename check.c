#undef NDEBUG

#include<assert.h>
#include<string.h>
#include<stdio.h>

#include"jsb.h"

char *pass[] = {
	"true",
	"false",
	"null",
	"0",
	"1",
	"-0",
	"1.2",
	"3e4",
	"5.6e-7",
	"\"\"",
	"\"foo\\tbar\"",
	"[]",
	"[1]",
	"[2,3]",
	"[4,[],5]",
	"[{\"\":[{}]}]",
	"{}",
	"{\"foo\":1}",
	"{\"foo\":1,\"bar\":2}",
};

typedef struct {
	char *key;
	char *value;
} subchk_t;

subchk_t subs[] = {
	{ "null", "null" },
	{ "false", "false" },
	{ "true", "true" },
	{ "number", "3.14159" },
	{ "string", "\"foo\"" },
	{ "empty_arr", "[]" },
	{ "empty_obj", "{}" },
	{ "complex_arr", "[0,\"abc\",{},true,\"\"]" },
	{ "complex_obj", "{\"abc\":[1,false,null,[]]}" },
};

static size_t _jsb_inc(char *dst, size_t dmax, const char *src, size_t slen, uint32_t flags){
	size_t dlen = 0;
	size_t rv, ret = 0;
	jsb_t jsb;
	int r;
	r = jsb_init(&jsb, dlen, flags);
	assert(0 == r);
	r = jsb_srclen(&jsb, !!slen);
	if(slen)
		slen--;
	assert(0 == r);
again:
	rv = jsb_update(&jsb, dst, src);
	switch(rv){
		default:
			assert(!ret);
			ret = rv;
			goto again;
		case JSB_FULL:
			assert(dlen != dmax);
			rv = jsb_dstlen(&jsb, dst, ++dlen);
			assert(rv == dlen);
			goto again;
		case JSB_FILL:
			r = jsb_srclen(&jsb, slen ? slen--,src++,1 : 0);
			assert(0 == r);
			goto again;
		case JSB_ERROR:
			ret = JSB_ERROR;
			break;
		case JSB_EOF:
			break;
	}
	assert(ret <= JSB_SIZE_MAX);
	return ret;
}

static size_t jsb_inc(void *dst, size_t dmax, const void *src, size_t slen, uint32_t flags){
	return _jsb_inc(dst, dmax, src, slen, flags);
}

static size_t cpy(void *dst, const void *src){
	const char *s = src;
	char *d = dst;
	while(*s)
		*d++ = *s++;
	return d - (char *)dst;
}

#define COUNT(array) (sizeof(array) / sizeof(*array))

static int chk_match(void){
	char *keys[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten" };
	size_t keyinfo[COUNT(keys) * 2 + 1] = { COUNT(keys) };
	/* runs checks internally */
	jsb_prepare(keyinfo, (const void **)keys, 1);
	return 0;
}

static void chk_analyze(uint8_t *bin){
	size_t meta[256];
	size_t c0, c1, s0, s1;
	size_t i, n = jsb_analyze(bin, 0, meta, sizeof(meta)/sizeof(*meta), 0);
	assert(n <= sizeof(meta)/sizeof(*meta));
	for(i = 0; bin[i] != JSB_DOC_END; i++){
		if(bin[i] >= 0xf5 && bin[i] < 0xfc){
			s0 = jsb_size(bin, i, NULL);
			s1 = jsb_size(bin, i, meta);
			assert(s0 == s1);
			c0 = jsb_count(bin, i, NULL);
			c1 = jsb_count(bin, i, meta);
			assert(c0 == c1);
		}
	}
}

int main(void){
	const int npass = sizeof(pass) / sizeof(*pass);
	const int nsubs = sizeof(subs) / sizeof(*subs);
	int r, i;
	char txt[1024], tmp[1024];
	uint8_t bin[1024];
	size_t len, plen, blen, off;

	for(i = 0; i < npass; i++){
		memset(bin, -1, sizeof(bin));
		memset(txt, 0xc0, sizeof(txt));
		memset(tmp, 0xc1, sizeof(tmp));
		plen = strlen(pass[i]);
		len = jsb(bin, sizeof(bin), pass[i], plen, 0);
		assert(jsb_inc(tmp, sizeof(tmp), pass[i], plen, 0) == len);
		assert(memcmp(bin, tmp, len) == 0);
		assert(len);
		blen = len;
		len = jsb(txt, sizeof(txt), bin, blen, JSB_REVERSE);
		assert(jsb_inc(tmp, sizeof(tmp), bin, blen, JSB_REVERSE) == len);
		assert(memcmp(txt, tmp, len + 1) == 0);
		assert(txt[len] == 0);
		assert(plen == len);
		r = memcmp(pass[i], txt, len);
		assert(!r);
	}

	/* build binary object from the above key/value list */
	blen = 0;
	bin[blen++] = JSB_OBJ;
	for(i = 0; i < nsubs; i++){
		bin[blen++] = JSB_KEY;
		blen += cpy(bin + blen, subs[i].key);
		len = jsb(bin + blen, sizeof(bin) - blen, subs[i].value, strlen(subs[i].value), 0);
		assert(len <= JSB_SIZE_MAX && len > 0);
		blen += --len;
		assert(bin[blen] == JSB_DOC_END);
	}
	bin[blen++] = JSB_OBJ_END;
	bin[blen++] = JSB_DOC_END;

	/* ensure it decodes properly */
	len = jsb(txt, sizeof(txt), bin, blen, JSB_REVERSE);
	assert(len <= JSB_SIZE_MAX && len > 0);

	/* now check that retrieving each key results in the original input */
	for(i = 0; i < nsubs; i++){
		off = jsb_obj_get(bin, 0, NULL, subs[i].key, strlen(subs[i].key));
		assert(off);
		len = jsb(txt, sizeof(txt), bin + off, jsb_size(bin, off, NULL) + 1, JSB_REVERSE);
		assert(len <= JSB_SIZE_MAX);
		assert(strlen(subs[i].value) == len);
		assert(memcmp(subs[i].value, txt, len) == 0);
	}

	chk_analyze(bin);

	/* build binary array from the above key/value list */
	blen = 0;
	bin[blen++] = JSB_ARR;
	for(i = 0; i < nsubs; i++){
		len = jsb(bin + blen, sizeof(bin) - blen, subs[i].value, strlen(subs[i].value), 0);
		assert(len <= JSB_SIZE_MAX && len > 0);
		blen += --len;
		assert(bin[blen] == JSB_DOC_END);
	}
	bin[blen++] = JSB_ARR_END;
	bin[blen++] = JSB_DOC_END;

	/* ensure it decodes properly */
	len = jsb(txt, sizeof(txt), bin, blen, JSB_REVERSE);
	assert(len <= JSB_SIZE_MAX && len > 0);

	/* now check that retrieving each key results in the original input */
	for(i = 0; i < nsubs; i++){
		off = jsb_arr_get(bin, 0, NULL, i);
		assert(off);
		len = jsb(txt, sizeof(txt), bin + off, jsb_size(bin, off, NULL) + 1, JSB_REVERSE);
		assert(len <= JSB_SIZE_MAX);
		assert(strlen(subs[i].value) == len);
		assert(memcmp(subs[i].value, txt, len) == 0);
	}

	chk_analyze(bin);

	chk_match();

	return 0;
}
