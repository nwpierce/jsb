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
	jsb_t jsb;
	size_t rv;
	jsb_init(&jsb, flags, sizeof(jsb));
	jsb.next_in = (void *)src;
	jsb.next_out = (void *)dst;
	jsb.avail_out = dmax;
	jsb.avail_in = 0;
	do{
		if(!jsb.avail_in){
			if(slen)
				jsb.avail_in++, slen--;
			else
				jsb_eof(&jsb);
		}else if(!jsb.avail_out){
			assert(dmax);
			jsb.avail_out++;
			dmax--;
		}
		rv = jsb_update(&jsb);
	}while(JSB_OK == rv);
	assert(rv == JSB_DONE);
	return jsb.total_out;
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

/* jsb_cmp result encoding is (cmp << 1) | 1, or 0 when not comparable */
static int cmp_txt(const char *a, const char *b, size_t offa, size_t offb){
	uint8_t ba[256], bb[256];
	size_t n;
	/* distinct fill so a comparison that runs past a terminator can't pass by luck */
	memset(ba, 0x01, sizeof(ba));
	memset(bb, 0x02, sizeof(bb));
	n = jsb(ba, sizeof(ba), a, strlen(a), 0, -1);
	assert(n <= JSB_SIZE_MAX);
	n = jsb(bb, sizeof(bb), b, strlen(b), 0, -1);
	assert(n <= JSB_SIZE_MAX);
	return jsb_cmp(ba, offa, bb, offb);
}

#define CMP(a, b, want) do{ \
	assert(cmp_txt(a, b, 0, 0) == (((want) << 1) | 1)); \
	assert(cmp_txt(b, a, 0, 0) == ((-(want) << 1) | 1)); \
}while(0)

static void chk_cmp(void){
	size_t i, j, k;
	/* ascending: each entry is strictly less than every later entry */
	char *strs[] = {
		"\"\"", "\"\\u0000\"", "\"a\"", "\"a\\u0000\"", "\"ab\"", "\"b\"", "\"\\u00e9\"", "\"\\ud800\\udc00\"",
	};
	char *nums[] = {
		"-1e50", "-100", "-10", "-2", "-1.5", "-1", "-0.5", "-1e-9", "-0.2e-9", "-1e-50",
		"0",
		"1e-50", "0.15e-49", "2e-50", "1e-10", "0.2e-9", "1e-9", "0.01", "0.1", "0.5", "1", "1.5", "2", "10", "100", "1e50",
	};
	/* equal spellings */
	char *eq[][3] = {
		{"0", "-0", "0.000e5"}, {"1", "1.0", "10e-1"}, {"-1", "-1.0", "-0.1e1"}, {"120", "1.2e2", "0.12E+3"},
		{"1e-10", "0.1e-9", "10e-11"}, {"1e-50", "0.1e-49", "100e-52"}, {"1e10", "0.1e11", "10e9"},
	};

	for(i = 0; i < COUNT(strs); i++){
		CMP(strs[i], strs[i], 0);
		for(j = i + 1; j < COUNT(strs); j++)
			CMP(strs[i], strs[j], -1);
	}
	for(i = 0; i < COUNT(nums); i++){
		CMP(nums[i], nums[i], 0);
		for(j = i + 1; j < COUNT(nums); j++)
			CMP(nums[i], nums[j], -1);
	}
	for(i = 0; i < COUNT(eq); i++)
		for(j = 0; j < 3; j++)
			for(k = 0; k < 3; k++)
				CMP(eq[i][j], eq[i][k], 0);

	/* keys compare like strings, across string/key too */
	{
		uint8_t ka[64], kb[64], sa[64];
		memset(ka, 0x01, sizeof(ka));
		memset(kb, 0x02, sizeof(kb));
		memset(sa, 0x03, sizeof(sa));
		assert(jsb(ka, sizeof(ka), "{\"a\":0}", 7, 0, -1) <= JSB_SIZE_MAX);
		assert(jsb(kb, sizeof(kb), "{\"ab\":0}", 8, 0, -1) <= JSB_SIZE_MAX);
		assert(jsb(sa, sizeof(sa), "\"a\"", 3, 0, -1) <= JSB_SIZE_MAX);
		assert(ka[1] == JSB_KEY && kb[1] == JSB_KEY);
		assert(jsb_cmp(ka, 1, kb, 1) == ((-1 * 2) | 1));
		assert(jsb_cmp(kb, 1, ka, 1) == ((1 * 2) | 1));
		assert(jsb_cmp(sa, 0, kb, 1) == ((-1 * 2) | 1));
		assert(jsb_cmp(kb, 1, sa, 0) == ((1 * 2) | 1));
		assert(jsb_cmp(ka, 1, sa, 0) == 1);
	}

	CMP("null", "null", 0);
	CMP("false", "true", -1);
	CMP("true", "true", 0);

	/* different or non-scalar types are not comparable */
	assert(cmp_txt("1", "\"1\"", 0, 0) == 0);
	assert(cmp_txt("null", "false", 0, 0) == 0);
	assert(cmp_txt("[]", "[]", 0, 0) == 0);
	assert(cmp_txt("{}", "{}", 0, 0) == 0);
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
		len = jsb(bin, sizeof(bin), pass[i], plen, 0, -1);
		assert(jsb_inc(tmp, sizeof(tmp), pass[i], plen, 0) == len);
		assert(memcmp(bin, tmp, len) == 0);
		assert(len);
		blen = len;
		len = jsb(txt, sizeof(txt), bin, blen, JSB_REVERSE, -1);
		assert(JSB_ERROR != len);
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
		len = jsb(bin + blen, sizeof(bin) - blen, subs[i].value, strlen(subs[i].value), 0, -1);
		assert(len <= JSB_SIZE_MAX && len > 0);
		blen += --len;
		assert(bin[blen] == JSB_DOC_END);
	}
	bin[blen++] = JSB_OBJ_END;
	bin[blen++] = JSB_DOC_END;

	/* ensure it decodes properly */
	len = jsb(txt, sizeof(txt), bin, blen, JSB_REVERSE, -1);
	assert(len <= JSB_SIZE_MAX && len > 0);

	/* now check that retrieving each key results in the original input */
	for(i = 0; i < nsubs; i++){
		off = jsb_obj_get(bin, 0, NULL, subs[i].key, strlen(subs[i].key));
		assert(off);
		len = jsb(txt, sizeof(txt), bin + off, jsb_size(bin, off, NULL) + 1, JSB_REVERSE, -1);
		assert(len <= JSB_SIZE_MAX);
		assert(strlen(subs[i].value) == len);
		assert(memcmp(subs[i].value, txt, len) == 0);
	}

	chk_analyze(bin);

	/* build binary array from the above key/value list */
	blen = 0;
	bin[blen++] = JSB_ARR;
	for(i = 0; i < nsubs; i++){
		len = jsb(bin + blen, sizeof(bin) - blen, subs[i].value, strlen(subs[i].value), 0, -1);
		assert(len <= JSB_SIZE_MAX && len > 0);
		blen += --len;
		assert(bin[blen] == JSB_DOC_END);
	}
	bin[blen++] = JSB_ARR_END;
	bin[blen++] = JSB_DOC_END;

	/* ensure it decodes properly */
	len = jsb(txt, sizeof(txt), bin, blen, JSB_REVERSE, -1);
	assert(len <= JSB_SIZE_MAX && len > 0);

	/* now check that retrieving each key results in the original input */
	for(i = 0; i < nsubs; i++){
		off = jsb_arr_get(bin, 0, NULL, i);
		assert(off);
		len = jsb(txt, sizeof(txt), bin + off, jsb_size(bin, off, NULL) + 1, JSB_REVERSE, -1);
		assert(len <= JSB_SIZE_MAX);
		assert(strlen(subs[i].value) == len);
		assert(memcmp(subs[i].value, txt, len) == 0);
	}

	chk_analyze(bin);

	chk_match();

	chk_cmp();

	return 0;
}
