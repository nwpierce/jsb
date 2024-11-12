#if RELEASE
#undef NDEBUG
#define NDEBUG
#undef DEBUG
#else
#include<string.h>
#endif

#include"jsb.h"

#include<sys/types.h>
#include<alloca.h>

#ifdef NDEBUG
#define assert(cond) ((void)0)
#else
#include<assert.h>
#include<stdarg.h>
#endif

#ifndef PRIVATE
#define PRIVATE static
#endif

#if RELEASE || !defined(DEBUG)
#define debug(args) do{}while(0)
#define GOTO(x) goto x
#else
#include<stdio.h>
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define debug(args) do{ fprintf(stderr, "%d: ", __LINE__); dbg args; }while(0)
static void dbg(char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#define GOTO(x) do{ debug(("goto %s\n", #x)); goto x; }while(0)
#endif

#ifndef STATIC_ASSERT
#if defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 201112L
#define STATIC_ASSERT(e, m) _Static_assert((e), m)
#else
#define ASSERT_CONCAT_(a, b) a##b
#define ASSERT_CONCAT(a, b) ASSERT_CONCAT_(a, b)
#define STATIC_ASSERT(e, m) enum { ASSERT_CONCAT(assert_line_, __LINE__) = 1/(int)(!!(e)) }
#endif
#endif

/* bytes 0xc0 and 0xc1 are both invalid in JSON as well as our binary format
 * we'll borrow one to use internally as the EOF marker */
#define JSB_INT_EOF 0xc1

/*#define PICK(x, a, b) ((x) ? a : b)*/
/*#define PICK(x, a, b) ((a) ^ (!(x) * ((a)^(b))))*/
#define PICK(x, a, b) ((b) ^ (!!(x) * ((a)^(b))))
/*#define PICK(x, a, b) ((b) ^ ((x) * ((a)^(b))))*/

#define XND 8 /* xor to toggle JSB_ARR/JSB_OBJ to JSB_ARR_END/JSB_OBJ_END */
#define XAO 1 /* xor to toggle JSB_ARR/JSB_ARR_END to JSB_OBJ/JSB_OBJ_END */

STATIC_ASSERT(JSB_OBJ     + 1 == JSB_ARR,     "chk: obj + 1 == arr");
STATIC_ASSERT(JSB_OBJ_END + 1 == JSB_ARR_END, "chk: obj_end + 1 == arr_end");
STATIC_ASSERT((JSB_ARR ^ JSB_ARR_END) == XND, "chk: arr ^ arr_end == XND");
STATIC_ASSERT((JSB_OBJ ^ JSB_OBJ_END) == XND, "chk: obj ^ obj_end == XND");
STATIC_ASSERT((JSB_ARR & XAO) == 0,           "chk: arr & XAO == 0");
STATIC_ASSERT((JSB_OBJ & XAO) == XAO,         "chk: obj & XAO == XAO");

STATIC_ASSERT(('a' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT(('l' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT(('s' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT(('e' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT(('r' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT(('u' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT(('l' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT((']' & 0x80) == 0, "assume 7-bit ascii");
STATIC_ASSERT(('}' & 0x80) == 0, "assume 7-bit ascii");

/* the _jsb_update() function is designed as a coroutine
 * (see: https://www.chiark.greenend.org.uk/~sgtatham/coroutines.html)
 * but use the gcc/clang __COUNTER__ macro instead of ANSI C's __LINE__ */

/* ensure the __COUNTER__ macro provides a increasing numeric sequence: */
STATIC_ASSERT(__COUNTER__ + 1 == __COUNTER__, "check counter");
/* if it doesn't, you can a)
 *	in the build system, try something along the lines of:
 *	gcc -D__COUNTER__=__COUNTER__ -E jsb.c | perl -lnpe 's/\b__COUNTER__\b/$i++/ge' | gcc -O3 -xc -c -o jsb.o -
 * or b)
 *	switch to using __LINE__, and
 *	use a larger size integer for jsb->state
 */

/*
 * Utility functions
 */

static int mcmp(const void *s1, const void *s2, size_t n){
	const uint8_t *a = s1, *b = s2;
	int r;
	while(n--)
		if((r = *a++ - *b++))
			return (r > 0) - (r < 0);
	return 0;
}

static size_t strsz(const void *s){
	const uint8_t *c = s;
	size_t n;
	assert(s);
	while(*c)
		c++;
	n = c - (const uint8_t *)s;
#if !RELEASE
	assert(strlen(s) == n);
#endif
	return n;
}


/* ensure ASCII-based assumptions are valid */

/* for space() below */
STATIC_ASSERT('\t' < ' ', "tab");
STATIC_ASSERT('\r' < ' ', "cr");
STATIC_ASSERT('\n' < ' ', "lf");

/* for digit()/pdigit()/hex()/nibble() below */
STATIC_ASSERT('0' + 1 == '1', "01");
STATIC_ASSERT('1' + 1 == '2', "12");
STATIC_ASSERT('2' + 1 == '3', "23");
STATIC_ASSERT('3' + 1 == '4', "34");
STATIC_ASSERT('4' + 1 == '5', "45");
STATIC_ASSERT('5' + 1 == '6', "56");
STATIC_ASSERT('6' + 1 == '7', "67");
STATIC_ASSERT('7' + 1 == '8', "78");
STATIC_ASSERT('8' + 1 == '9', "89");

/* for hex()/nibble() below */
STATIC_ASSERT('a' + 1 == 'b', "ab");
STATIC_ASSERT('b' + 1 == 'c', "bc");
STATIC_ASSERT('c' + 1 == 'd', "cd");
STATIC_ASSERT('d' + 1 == 'e', "de");
STATIC_ASSERT('e' + 1 == 'f', "ef");

/* for hex() below */
STATIC_ASSERT('A' + 1 == 'B', "AB");
STATIC_ASSERT('B' + 1 == 'C', "BC");
STATIC_ASSERT('C' + 1 == 'D', "CD");
STATIC_ASSERT('D' + 1 == 'E', "DE");
STATIC_ASSERT('E' + 1 == 'F', "EF");

/* return 1 if supplied character is JSON-flavored whitespace */
PRIVATE uint8_t space(uint8_t ch){
	if(ch > ' ') return 0;
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/* digit */
PRIVATE uint8_t digit(uint8_t ch){
	return ch >= '0' && ch <= '9';
}

/* positive digit */
PRIVATE uint8_t pdigit(uint8_t ch){
	return ch >= '1' && ch <= '9';
}

/* map hexidecimal character to numeric value or -1 on error */
PRIVATE int hex(uint8_t ch){
	if(ch >= '0' && ch <= '9')
		return ch - '0';
	if(ch >= 'a' && ch <= 'f')
		return ch - ('a' - 0xa);
	if(ch >= 'A' && ch <= 'F')
		return ch - ('A' - 0xa);
	return -1;
}

/* map lower 4 bits to hexidecimal character */
PRIVATE uint8_t nibble(uint8_t ch){
	ch &= 0xf;
	return ch + (ch < 0xa ? '0' : 'a' - 0xa);
}

/* generic heap routines */

typedef int heap_test(void *data, size_t a, size_t b);
typedef void heap_swap(void *data, size_t a, size_t b);

PRIVATE void heap_siftdown(void *data, size_t n, heap_test test, heap_swap swap, size_t i){
	size_t t = i;
	size_t a = 2 * i + 1;
	size_t b = 2 * i + 2;
	if (n > a && test(data, t, a))
		t = a;
	if (n > b && test(data, t, b))
		t = b;
	if(i != t){
		swap(data, i, t);
		heap_siftdown(data, n, test, swap, t);
	}
}

PRIVATE void heap_ify(void *data, size_t n, heap_test test, heap_swap swap){
	size_t i = n / 2;
	while(i--)
		heap_siftdown(data, n, test, swap, i);
}

PRIVATE void heap_sort(void *data, size_t n, heap_test test, heap_swap swap){
	heap_ify(data, n, test, swap);
	while(n-- > 1){
		swap(data, 0, n);
		heap_siftdown(data, n, test, swap, 0);
	}
}

/* indexing support */

#define IDX_HDR 2
#define IDX_FTR 1
#define IDX_PAD (IDX_HDR + IDX_FTR)

typedef size_t node_t[3];

PRIVATE void swap_nodes(void *data, size_t a, size_t b){
	node_t *nodes = data;
	size_t i, t;
	assert(a != b);
	for(i = 0; i < 3; i++){
		t = nodes[a][i];
		nodes[a][i] = nodes[b][i];
		nodes[b][i] = t;
	}
}

/* used for deciding what to keep in the index - minheap by size/count */
PRIVATE int sz_a_gt_b(node_t a, node_t b){
	assert(a != b);
	return a[1] > b[1] || (a[1] == b[1] && a[2] > b[2]);
}

/* wrapper for the above */
PRIVATE int hs_sz_a_gt_b(void *data, size_t a, size_t b){
	node_t *nodes = data;
	return sz_a_gt_b(nodes[a], nodes[b]);
}

/* used for second pass - maxheap by offset */
PRIVATE int hs_off_a_lt_b(void *data, size_t a, size_t b){
	node_t *nodes = data;
	assert(a != b);
	return nodes[a][0] < nodes[b][0];
}

PRIVATE void idx_insert(size_t n, size_t *idx, node_t v){
	node_t *arr = (node_t *)(idx + IDX_HDR);
	size_t j, i = idx[0];
	if(i < n){
		/* quick append if index isn't full */
		for(j = 0; j < 3; j++)
			arr[i][j] = v[j];
		idx[0] = ++i;
		return;
	}
	if(i == n){
		/* heapify records if we've filled the array and have more to insert */
		heap_ify(arr, n, hs_sz_a_gt_b, swap_nodes);
		idx[0] = SIZE_MAX;
	}
	/* if current value is bigger than the smallest one in the index */
	if(sz_a_gt_b(v, arr[0])){
		/* first overwrite root node */
		for(j = 0; j < 3; j++)
			arr[0][j] = v[j];
		/* then fix the heap */
		heap_siftdown(arr, n, hs_sz_a_gt_b, swap_nodes, 0);
	}
}

PRIVATE size_t idx_finish(size_t *idx, size_t n){
	size_t i = (n < idx[0]) ? (idx[0] = n) : (n = idx[0]);
	node_t *arr = (node_t *)(idx + IDX_HDR);
	/* now heapsort on byte-offset column, low -> high */
	heap_sort(arr, i, hs_off_a_lt_b, swap_nodes);
	/* and cap it with an out-of-range offset */
	arr[i][0] = SIZE_MAX;
	return n;
}

/* return first index node >= supplied offset, possibly off=SIZE_MAX */
PRIVATE const size_t *idx_seek(const size_t *idx, size_t off){
	size_t ret, mid, lo = 0, hi = ret = idx[0];
	node_t *nodes = (node_t *)(idx + IDX_HDR);
	while(lo < hi){
		mid = (lo >> 1) + (hi >> 1) + (lo & hi & 1);
		if(nodes[mid][0] < off)
			ret = lo = mid + 1;
		else
			ret = hi = mid;
	}
	assert(nodes[ret][0] >= off);
	return (size_t *)nodes[ret];
}

PRIVATE const size_t *idx_find(const size_t *idx, size_t off){
	const size_t *r = NULL;
	if(idx && idx[0]){
		r = idx_seek(idx, off);
		if(r[0] != off)
			r = NULL;
	}
	return r;
}

PRIVATE size_t _jsb_str_count(const uint8_t *bin, size_t off, size_t *endpos){
	size_t c = 0;
	uint8_t b = bin[off];
	while(b < 0xf5){
		c += ((b & 0xc0) != 0x80);
		b = bin[++off];
	}
	if(endpos)
		*endpos = off;
	return c;
}

PRIVATE size_t _jsb_size(const void *base, size_t offset, const size_t *meta);

PRIVATE size_t idx_load(const uint8_t *bin, size_t off, size_t *idx, const size_t n, const size_t m, size_t d){
	uint8_t t = bin[off];
	node_t v = { 0, 0, 0 };
	v[0] = off++;
	assert(t > 0xf4 && t < 0xfd);
	switch(t){
		default:
			off = JSB_ERROR;
		case JSB_NULL:
		case JSB_FALSE:
		case JSB_TRUE:
			goto done;
		case JSB_NUM:
			/* we could store something else in v[2] for integers, as character count is size - 1.
			   Some options include:
			   * offset to first character that requires floating point parsing ('e' or '.'), else zero
			   * absolute value of an integer if it fit within the platform's size_t, else zero
			   * something trickier?
			 */
		case JSB_STR:
		case JSB_KEY:
			v[2] = _jsb_str_count(bin, off, &off);
			break;
		case JSB_OBJ:
		case JSB_ARR:
			t ^= XND;
			d++;
			if(bin[off] != t){
				if(idx[1] < d)
					idx[1] = d;
				do{
					if(JSB_OBJ_END == t){
						assert(bin[off] == JSB_KEY);
						off = idx_load(bin, off, idx, n, m, d);
						if(JSB_ERROR == off) goto done;
					}
					assert(bin[off] != JSB_KEY);
					assert(bin[off] != JSB_ARR_END);
					assert(bin[off] != JSB_OBJ_END);
					assert(bin[off] != JSB_DOC_END);
					off = idx_load(bin, off, idx, n, m, d);
					if(JSB_ERROR == off) goto done;
					v[2]++;
				}while(bin[off] != t);
			}
			assert(bin[off] == t);
			off++;
			d--;
	}
	assert(bin[off] > 0xf4);
	v[1] = off - v[0];
#if CHECK
	assert(_jsb_size(bin, v[0], NULL) == v[1]);
#endif
	if(v[1] >= m)
		idx_insert(n, idx, v);
done:
	assert(off != JSB_ERROR);
	return off;
}


/*
 * Parser API
 */

/* return number of jsb_unit_t's required to back a jsb_t for a given maximum json depth */
PRIVATE size_t __attribute__((const)) _jsb_units(size_t maxdepth);
JSB_API size_t __attribute__((const))  jsb_units(size_t maxdepth){ return _jsb_units(maxdepth); }
PRIVATE size_t __attribute__((const)) _jsb_units(size_t maxdepth){
	const size_t bc = ((maxdepth == (size_t)-1 ? JSB_DEFAULT_STACK_BYTES : maxdepth) + 7) / 8;
	return (JSB_SIZE + bc + sizeof(jsb_unit_t) - 1) / sizeof(jsb_unit_t);
}

/* initialize jsb parser state */
PRIVATE size_t _jsb_init(jsb_t *jsb, uint32_t flags, size_t jsbsize);
JSB_API size_t  jsb_init(jsb_t *jsb, uint32_t flags, size_t jsbsize){ return _jsb_init(jsb, flags, jsbsize); }
PRIVATE size_t _jsb_init(jsb_t *jsb, uint32_t flags, size_t jsbsize){
	size_t stackbytes = (jsbsize < JSB_SIZE) ? JSB_DEFAULT_STACK_BYTES : (jsbsize - JSB_SIZE);
	debug(("jsb_init(%p, %zx, %zu)\n", (void *)jsb, (size_t)flags, jsbsize));

	/* don't modify next_in/avail_in/next_out/avail_out */
	jsb->total_out = 0;
	jsb->total_in = 0;
	jsb->depth = 0;
	*(size_t *)&jsb->maxdepth = stackbytes * 8;
	jsb->flag_eof = !!(flags & JSB_EOF);
	jsb->flag_reverse = !!(flags & JSB_REVERSE);
	jsb->flag_ascii = !!(flags & JSB_ASCII);
	jsb->flag_lines = !!(flags & JSB_LINES);
	jsb->obj = 0;
	jsb->key = 0;
	jsb->misc = 0;
	jsb->code = 0;
	jsb->state = 0;
	jsb->ch = 0;
	jsb->outb = JSB_INT_EOF;
	while(stackbytes--)
		jsb->stack[stackbytes] = 0;
	return jsb->maxdepth;
}

#define J(x) j_ ## x

#define JUMP(target) GOTO(J(target))

/* see: https://stackoverflow.com/questions/36932774/reset-counter-macro-to-zero */
enum { CB = __COUNTER__ };

#define YIELD(sz) do{                          \
	enum { ctr = __COUNTER__ - CB };           \
	jsb->state = ctr;                          \
	ret = sz;                                  \
	goto yield;                                \
	case ctr: break;                           \
}while(0)

#define BEGIN(x) switch(x){ default: ERROR; case 0

#define END }

#define ADDCH APPEND(jsb->ch)

#define APPEND(x) do{                          \
	const uint8_t t = (x);                     \
	debug(("append: %02x\n", t));              \
	assert(JSB_INT_EOF != t);                  \
	if(dstpos != dstlen){                      \
		dst[dstpos++] = t;                     \
	}else{                                     \
		jsb->outb = t;                         \
		YIELD(JSB_OK);                         \
	}                                          \
}while(0)

/* tag labels with line number for use within a macro */
#define __tag(x, y) x ## y
#define _tag(x, y) __tag(x, y)
#define tag(x) _tag(x, __LINE__)

#define NEXT(sw) do{                           \
	debug(("next!\n"));                        \
tag(next):                                     \
	if(srcpos != srclen){                      \
		jsb->ch = src[srcpos++];               \
		if(sw && space(jsb->ch))               \
			goto tag(next);                    \
		if(0xc0 == jsb->ch || 0xc1 == jsb->ch) \
		    ERROR;                             \
		debug(("ch: %02x\n", jsb->ch));        \
	}else if(jsb->flag_eof){                   \
		debug(("ch: EOF\n"));                  \
		jsb->ch = JSB_INT_EOF;                 \
	}else{                                     \
		YIELD(JSB_OK);                         \
		goto tag(next);                        \
	}                                          \
}while(0)

#if RELEASE
#define ERROR goto error
#else
#define ERROR do{ jsb->code = __LINE__; goto error; }while(0)
#endif

PRIVATE size_t _jsb_update(jsb_t *jsb){
	size_t ret;

	size_t srcpos = 0;
	size_t dstpos = 0;

	const size_t srclen = jsb->avail_in;
	const size_t dstlen = jsb->avail_out;
	const uint8_t * const src = jsb->next_in;
	uint8_t * const dst = jsb->next_out;

	if(0){ /* save state and suspend */
yield:
		debug(("yield: %d\n", ret));
		jsb->avail_in -= srcpos;
		jsb->avail_out -= dstpos;
		jsb->next_in += srcpos;
		jsb->next_out += dstpos;
		jsb->total_in += srcpos;
		jsb->total_out += dstpos;
		return ret;
	}

	if(jsb->outb != JSB_INT_EOF){
		assert(jsb->outb != 0xc0);
		if(dstlen)
			dst[dstpos++] = jsb->outb;
		else
			return JSB_OK;
		jsb->outb = JSB_INT_EOF;
	}

	debug(("enter: %d\n", jsb->state));

BEGIN(jsb->state):
	if(jsb->flag_reverse)
		goto reverse;
	JUMP(value);

error:
	debug(("error: line %d\n", jsb->code));
	while(1) YIELD(JSB_ERROR);

J(escape):
	NEXT(0);
	switch(jsb->ch){
		default: ERROR;
		case 't': jsb->ch = '\t'; break;
		case 'n': jsb->ch = '\n'; break;
		case 'r': jsb->ch = '\r'; break;
		case 'f': jsb->ch = '\f'; break;
		case 'b': jsb->ch = '\b'; break;
		case '"': case '/': case '\\':
			break;
		case 'u':
			jsb->code = 0;
			jsb->misc = 3;
			JUMP(hex);
	}
	ADDCH;
	JUMP(string2);

J(null):
	jsb->code = 'u' | ('l'<<8) | ('l'<<16);
	jsb->ch = JSB_NULL;
	JUMP(const);

J(false):
	jsb->code = 'a' | ('l'<<8) | ('s'<<16) | ('e'<<24);
	jsb->ch = JSB_FALSE;
	JUMP(const);

J(true):
	jsb->code = 'r' | ('u'<<8) | ('e'<<16);
	jsb->ch = JSB_TRUE;
	JUMP(const);

J(const):
	assert(jsb->code);
	ADDCH;
	do{
		NEXT(0);
		if((jsb->code & 0xff) != jsb->ch)
			ERROR;
	}while(jsb->code >>= 8);
	JUMP(more);

J(pop):
	APPEND(JSB_ARR_END - jsb->obj); /* JSB_ARR_END - 1 == JSB_OBJ_END */
	if(!jsb->depth--) ERROR;
	jsb->obj = (jsb->stack[jsb->depth >> 3] >> (jsb->depth & 7)) & 1;
	assert(jsb->obj < 2);
	jsb->key = 0;
	JUMP(more);

J(endnum):
	if(JSB_INT_EOF != jsb->ch){
		debug(("srcpos--\n"));
		srcpos--;
	}
	JUMP(more);

J(more):
	if(!jsb->depth)
		JUMP(done);
	debug(("more: obj/key = %u/%u\n", jsb->obj, jsb->key));
	NEXT(1);
	if(PICK(jsb->key, ':', ',') == jsb->ch){
		if(jsb->key ^= jsb->obj)
			JUMP(key);
		JUMP(value);
	}else if(PICK(jsb->obj, '}', ']') == jsb->ch){
		if(jsb->key)
			ERROR;
		JUMP(pop);
	}
	ERROR;

J(key):
	NEXT(1);
J(key2):
	if(jsb->ch != '"')
		ERROR;
	APPEND(JSB_KEY);
	if(0)
J(string):
		APPEND(JSB_STR);
J(string2):
	NEXT(0);
	if('"' == jsb->ch){
		JUMP(more);
	}else if('\\' == jsb->ch){
		JUMP(escape);
	}else if(jsb->ch >= 0x20 && jsb->ch < 0x80){
		ADDCH;
	}else if((jsb->ch & 0xe0) == 0xc0 && (jsb->ch & 0x1f)){
		jsb->code = jsb->ch & 0x1f;
		jsb->misc = 1;
		JUMP(unicode);
	}else if((jsb->ch & 0xf0) == 0xe0){
		jsb->code = jsb->ch & 0xf;
		jsb->misc = 2;
		JUMP(unicode);
	}else if((jsb->ch & 0xf8) == 0xf0){
		jsb->code = jsb->ch & 0x7;
		jsb->misc = 3;
		JUMP(unicode);
	}else ERROR;
	JUMP(string2);

J(push):
	APPEND(JSB_ARR - jsb->key); /* JSB_ARR - 1 == JSB_OBJ */
	NEXT(1);
	if(PICK(jsb->key, '}', ']') == jsb->ch){
		APPEND(JSB_ARR_END - jsb->key); /* JSB_ARR_END - 1 == JSB_OBJ_END */
		jsb->key = 0;
		JUMP(more);
	}
	if(jsb->maxdepth == jsb->depth) ERROR;
	{
		const uint8_t tmp = 1 << (jsb->depth & 7);
		if(jsb->obj)
			jsb->stack[jsb->depth>>3] |= tmp;
		else
			jsb->stack[jsb->depth>>3] &= ~tmp;
	}
	jsb->depth++;
	jsb->obj = jsb->key;
	if(jsb->key)
		JUMP(key2);
	JUMP(value2);

J(value):
	debug(("value!\n"));
	NEXT(1);
	if(0)
J(value2):
		debug(("value2!\n"));
	switch(jsb->ch){
		case '"': JUMP(string);
		case '1': case '2': case '3':
		case '4': case '5': case '6':
		case '7': case '8': case '9':
			JUMP(number);
		case '-': JUMP(sign);
		case '0': JUMP(zero);
		case 't': JUMP(true);
		case 'f': JUMP(false);
		case 'n': JUMP(null);
		case '{':
		case '[':
			if(jsb->key)
		default:
				ERROR;
			jsb->key = (jsb->ch == '{');
			JUMP(push);
	}

J(sign):
	APPEND(JSB_NUM);
	ADDCH;
	NEXT(0);
	if(pdigit(jsb->ch))
		JUMP(number2);
	if('0' == jsb->ch)
		JUMP(zero2);
	ERROR;

J(number):
	APPEND(JSB_NUM);
J(number2):
	ADDCH;
	NEXT(0);
	if(digit(jsb->ch))
		JUMP(number2);
	if('.' == jsb->ch)
		JUMP(decimal);
	JUMP(expchk);

J(zero):
	APPEND(JSB_NUM);
J(zero2):
	ADDCH;
	NEXT(0);
	if('.' == jsb->ch)
		JUMP(decimal);
	JUMP(expchk);

J(decimal):
	ADDCH;
	NEXT(0);
	if(digit(jsb->ch))
		JUMP(decimal_more);
	ERROR;

J(decimal_more):
	ADDCH;
	NEXT(0);
	if(digit(jsb->ch))
		JUMP(decimal_more);
	JUMP(expchk);

J(expchk):
	if('e' == jsb->ch || 'E' == jsb->ch)
		JUMP(exponent);
	JUMP(endnum);

J(exponent):
	APPEND('e');
	NEXT(0);
	jsb->misc = ('-' == jsb->ch);
	if('-' == jsb->ch || '+' == jsb->ch)
		NEXT(0);
	if(pdigit(jsb->ch))
		JUMP(exponent_more);
	else if(jsb->ch == '0')
		JUMP(exponent_zero);
	ERROR;

J(exponent_zero):
	NEXT(0);
	if(jsb->ch == '0')
		JUMP(exponent_zero);
	if(digit(jsb->ch))
		JUMP(exponent_more);
	APPEND('0');
	JUMP(endnum);

J(exponent_more):
	if(jsb->misc)
		APPEND('-');

J(exponent_more2):
	ADDCH;
	NEXT(0);
	if(digit(jsb->ch))
		JUMP(exponent_more2);
	JUMP(endnum);

J(unicode):
	ADDCH;
	NEXT(0);
	if((jsb->ch & 0xc0) != 0x80)
		ERROR;
	jsb->code = (jsb->code << 6) | (jsb->ch & 0x3f);
	if(--jsb->misc)
		JUMP(unicode);
	if(jsb->code > 0x10ffff || (jsb->code >= 0xd800 && jsb->code < 0xe000))
		ERROR;
	ADDCH;
	JUMP(string2);

J(hexb):
	NEXT(0);
	if('\\' != jsb->ch)
		ERROR;
	NEXT(0);
	if('u' != jsb->ch)
		ERROR;
	jsb->code <<= 16;
	jsb->misc = 3;
	JUMP(hex);

J(hex):
	NEXT(0);
	{
		uint32_t tmp = hex(jsb->ch);
		if(tmp > 0xf)
			ERROR;
		jsb->code |= tmp << (4 * jsb->misc);
		if(jsb->misc){
			jsb->misc--;
			JUMP(hex);
		}
		/* deal w/ UTF-16 surrogate pairs - https://en.wikipedia.org/wiki/UTF-16 */
		tmp = jsb->code & 0xfc00;
		if(jsb->code < 0x10000){
			if(0xdc00 == tmp)
				ERROR;
			if(0xd800 == tmp)
				JUMP(hexb);
		}else{
			if(0xdc00 != tmp)
				ERROR;
			jsb->code = 0x10000 + ((jsb->code & 0x3ff0000) >> 6) + (jsb->code & 0x3ff);
		}
	}
	if(jsb->code < 0x80){
	}else if(jsb->code < (0x1<<11)){
		jsb->code = 0x80c0 | (jsb->code >> 6) | ((jsb->code & 0x3f)<<8);
	}else if(jsb->code < (0x1<<16)){
		jsb->code = 0x8080e0 | (jsb->code >>12) | ((jsb->code & 0xfc0) << 2) | ((jsb->code & 0x3f) << 16);
	}else if(jsb->code < (0x11<<16)){
		jsb->code = 0x808080f0 | (jsb->code >> 18) | ((jsb->code & 0x3f000) >> 4) | ((jsb->code & 0xfc0) << 10) | ((jsb->code & 0x3f)<<24);
	}else ERROR;
	do{
		APPEND(jsb->code);
	}while(jsb->code >>= 8);
	JUMP(string2);

J(done):
	APPEND(JSB_DOC_END);
	NEXT(1);
	if(jsb->flag_lines){
/*		YIELD(JSB_OK); */
		if(JSB_INT_EOF != jsb->ch)
			JUMP(value2);
	}else{
		if(jsb->ch != JSB_INT_EOF)
			srcpos--;
	}
	while(1)
		YIELD(JSB_DONE);

#undef J
#define J(x) r_ ## x

reverse:
	NEXT(0);
	JUMP(start);

J(pop):
	jsb->depth--;
	jsb->misc = 0;
	if(0)
J(push):
		if(!++jsb->depth)
			ERROR;
	ADDCH;
J(nextch):
	NEXT(0);
J(next):
	if(!jsb->depth)
		JUMP(done);
	if(JSB_ARR_END == jsb->ch){
		jsb->ch = ']';
		JUMP(pop);
	}else if(JSB_OBJ_END == jsb->ch){
		jsb->ch = '}';
		JUMP(pop);
	}else if(JSB_KEY == jsb->misc){
		APPEND(':');
	}else if(JSB_ARR != jsb->misc && JSB_OBJ != jsb->misc){
		APPEND(',');
	}

J(start):
	jsb->code = 0;
	jsb->misc = jsb->ch;
	if(JSB_NUM == jsb->ch){
		while(1){
			NEXT(0);
			if(jsb->ch > 0xf4)
				JUMP(next);
			ADDCH;
		}
	}else if(JSB_ARR == jsb->ch){
		jsb->ch = '[';
		JUMP(push);
	}else if(JSB_OBJ == jsb->ch){
		jsb->ch = '{';
		JUMP(push);
	}else if(JSB_KEY == jsb->ch || JSB_STR == jsb->ch){
		APPEND('"');
		JUMP(string);
	}else if(JSB_TRUE == jsb->ch){
		jsb->ch = 't';
		jsb->code = 'r' | ('u'<<8) | ('e'<<16);
	}else if(JSB_FALSE == jsb->ch){
		jsb->ch = 'f';
		jsb->code = 'a' | ('l'<<8) | ('s'<<16) | ('e'<<24);
	}else if(JSB_NULL == jsb->ch){
		jsb->ch = 'n';
		jsb->code = 'u' | ('l'<<8) | ('l'<<16);
	}else{
		ERROR;
	}
	ADDCH;
	while(jsb->code){
		APPEND(jsb->code);
		jsb->code >>= 8;
	}
	JUMP(nextch);

J(string):
	NEXT(0);
	if(jsb->ch > 0xf4){
		APPEND('"');
		JUMP(next);
	}else if(jsb->ch >= 0x20 && jsb->ch != '"' && jsb->ch != '\\' && (!jsb->flag_ascii || jsb->ch < 0x80)){
		ADDCH;
		JUMP(string);
	}else if(jsb->ch < 0x80){
		APPEND('\\');
		if('"' == jsb->ch || '\\' == jsb->ch) (void)jsb->ch;
		else if('\t' == jsb->ch) jsb->ch = 't';
		else if('\n' == jsb->ch) jsb->ch = 'n';
		else if('\r' == jsb->ch) jsb->ch = 'r';
		else if('\f' == jsb->ch) jsb->ch = 'f';
		else if('\b' == jsb->ch) jsb->ch = 'b';
		else{
			APPEND('u');
			APPEND('0');
			APPEND('0');
			APPEND(nibble(jsb->ch>>4));
			jsb->ch = nibble(jsb->ch);
		}
	}else{
		if((jsb->ch & 0xf8) == 0xf0){
			jsb->code = jsb->ch & 0x7;
			goto _3;
		}
		if((jsb->ch & 0xf0) == 0xe0){
			jsb->code = jsb->ch & 0xf;
			goto _2;
		}
		assert((jsb->ch & 0xe0) == 0xc0);
		jsb->code = jsb->ch & 0x1f;
		goto _1;
		_3: NEXT(0); assert((jsb->ch & 0xc0) == 0x80); jsb->code <<= 6; jsb->code |= jsb->ch ^ 0x80;
		_2: NEXT(0); assert((jsb->ch & 0xc0) == 0x80); jsb->code <<= 6; jsb->code |= jsb->ch ^ 0x80;
		_1: NEXT(0); assert((jsb->ch & 0xc0) == 0x80); jsb->code <<= 6; jsb->code |= jsb->ch ^ 0x80;
		assert(jsb->code < 0x110000);
		APPEND('\\');
		APPEND('u');
		if(jsb->code < 0x10000){
			APPEND(nibble(jsb->code >> 12));
			APPEND(nibble(jsb->code >> 8));
			APPEND(nibble(jsb->code >> 4));
		}else{
			jsb->code -= 0x10000;
			APPEND('d');
			APPEND(nibble((jsb->code >> 18) | 0x8));
			APPEND(nibble(jsb->code >> 14));
			APPEND(nibble(jsb->code >> 10));
			APPEND('\\');
			APPEND('u');
			APPEND('d');
			APPEND(nibble((jsb->code >> 8) | 0xc));
			APPEND(nibble(jsb->code >> 4));
		}
		jsb->ch = nibble(jsb->code);
	}
	ADDCH;
	JUMP(string);

J(done):
	/* parsing successful */
	if(jsb->flag_lines)
		APPEND('\n');  /* append a newline */
	APPEND(0);       /* and null terminate */
	dstpos--; /* but don't include in output */
	debug(("done!\n"));
	if(jsb->flag_lines){
/*		YIELD(JSB_OK); */
J(again):
		NEXT(0);
		switch(jsb->ch){
			case JSB_DOC_END:
				JUMP(again);
			case JSB_NULL:
			case JSB_FALSE:
			case JSB_TRUE:
			case JSB_NUM:
			case JSB_STR:
			case JSB_KEY:
			case JSB_ARR:
			case JSB_OBJ:
				JUMP(start);
			default:
				srcpos--;
				/* fall through */
			case JSB_INT_EOF:
				break;
		}
	}
	while(1)
		YIELD(JSB_DONE);
END;
}

JSB_API size_t jsb_update(jsb_t *jsb){
	return _jsb_update(jsb);
}

JSB_API void jsb_eof(jsb_t *jsb){
	debug(("eof\n"));
	jsb->flag_eof = 1;
}

JSB_API size_t jsb(void *dst, size_t dstlen, const void *src, size_t srclen, uint32_t flags, size_t maxdepth){
	size_t md, ret;
#ifdef __TINYC__
	/* tcc doesn't provide alloca(), at least with -nostdlib -fno-builtin */
	jsb_unit_t ju[_jsb_units(maxdepth)];
	jsb_t *jsb = (jsb_t *)ju;
	size_t jsz = sizeof(ju);
#else
	/* c89 doesn't support dynamically sized arrays */
	size_t jsz = sizeof(jsb_unit_t) * _jsb_units(maxdepth);
	jsb_t *jsb = alloca(jsz);
#endif
	md = _jsb_init(jsb, flags | JSB_EOF, jsz);
	(void)md;
	jsb->next_in = src;
	jsb->next_out = dst;
	jsb->avail_in = srclen;
	jsb->avail_out = dstlen;
	ret = _jsb_update(jsb);
	if(ret != JSB_DONE){
		debug(("not done: %zu\n", ret));
		ret = JSB_ERROR;
	}else if(jsb->avail_in){
		debug(("leftovers: %zu\n", jsb->avail_in));
		ret = JSB_ERROR;
	}else{
		ret = jsb->total_out;
		debug(("done: %zu\n", ret));
	}
	return ret;
}


/*
 * binary inspection/traversal routines
 */

JSB_API size_t jsb_analyze(const void *base, size_t offset, size_t *idx, size_t n, size_t m){
	size_t r;
	if(n < IDX_PAD)
		return JSB_ERROR;
	n = (n - IDX_PAD) / 3;
	idx[0] = idx[1] = 0;
	r = idx_load(base, offset, idx, n, m, 0);
	if(JSB_ERROR != r)
		r = idx_finish(idx, n);
	return r;
}

PRIVATE uint8_t _jsb_type(const void *base, size_t offset);
JSB_API uint8_t  jsb_type(const void *base, size_t offset){ return _jsb_type(base, offset); }
PRIVATE uint8_t _jsb_type(const void *base, size_t offset){
	uint8_t t = *(offset + (uint8_t *)base);
	switch(t){
		default: t = 0; /* fall through */
		case JSB_NULL:
		case JSB_FALSE:
		case JSB_TRUE:
		case JSB_NUM:
		case JSB_STR:
		case JSB_KEY:
		case JSB_ARR:
		case JSB_OBJ:
			return t;
	}
}

JSB_API size_t jsb_bool(const void *base, size_t offset){
	uint8_t t = _jsb_type(base, offset);
	const uint8_t *bin = base;
	switch(t){
		default:        return JSB_ERROR;
		case JSB_NULL:
		case JSB_FALSE: return 0;
		case JSB_TRUE:  return 1;
		case JSB_NUM:   break;
		case JSB_STR:
		case JSB_KEY:   return bin[offset + 1] < 0xf5;
		case JSB_ARR:
		case JSB_OBJ:   return bin[offset + 1] == (t ^ XND) ? 0 : 1;
	}
	t = bin[++offset];
	offset += (t == '-');
	while(t = bin[offset++], t < 0xf5){
		if(t == '0' || t == '.' || t == '-') continue;
		assert((t >= '1' && t <= '9') || t == 'e');
		return t != 'e';
	}
	return 0;
}

JSB_API size_t  jsb_size(const void *base, size_t offset, const size_t *meta){ return _jsb_size(base, offset, meta); }
PRIVATE size_t _jsb_size(const void *base, size_t offset, const size_t *meta){
	const uint8_t * const v = offset + (uint8_t *)base;
	const uint8_t * c = v;
	size_t depth = 1;
	const size_t *m;
	uint8_t t = *c++;
	switch(t){
		default:
			return 0;
		case JSB_NULL:
		case JSB_FALSE:
		case JSB_TRUE:
			return 1;
		case JSB_NUM:
		case JSB_STR:
		case JSB_KEY:
		case JSB_ARR:
		case JSB_OBJ:
			break;
	}
	m = idx_find(meta, offset);
	if(m) return m[1];
	switch(t){
		case JSB_NUM:
		case JSB_STR:
		case JSB_KEY:
			while(*c < 0xf5)
				c++;
			return c - v;
	}
again:
	switch(*c++){
		case JSB_ARR:
		case JSB_OBJ:
			depth++;
			/* fall through */
		default:
			goto again;
		case JSB_ARR_END:
		case JSB_OBJ_END:
			if(--depth)
				goto again;
			return c - v;
		case JSB_DOC_END:
		case 0xc0:
		case 0xc1:
			return 0;
	}
}

JSB_API size_t jsb_count(const void *base, size_t offset, const size_t *meta){
	const size_t *m;
	size_t sz, n = 0;
	const uint8_t *bin = base;
	uint8_t t = bin[offset];
	switch(t){
		default:        return JSB_ERROR;
		case JSB_NULL:
		case JSB_FALSE:
		case JSB_TRUE:  return 0;
		case JSB_NUM:
		case JSB_STR:
		case JSB_KEY:
		case JSB_ARR:
		case JSB_OBJ:   break;
	}
	m = idx_find(meta, offset++);
	if(m) return m[2];
	switch(t){
		case JSB_NUM:
		case JSB_STR:
		case JSB_KEY:   return _jsb_str_count(bin, offset, NULL);
	}
	/* xlate JSB_ARR/JSB_OBJ to JSB_ARR_END/JSB_OBJ_END and look for that */
	t ^= XND;
	while(bin[offset] != t){
		sz = _jsb_size(base, offset, NULL);
		/* sz = _jsb_size(base, offset, meta); */
		assert(sz);
		if(!sz)
			return JSB_ERROR;
		offset += sz;
		n++;
	}
	return n >> (t&XAO);
}

JSB_API size_t jsb_arr_get(const void *base, size_t offset, const size_t *meta, size_t idx){
	size_t sz;
	uint8_t *c = offset + (uint8_t *)base;
	if(JSB_ARR != *c++)
		return 0;
	while(idx--){
		sz = _jsb_size(c, 0, meta);
		if(!sz) return 0;
		assert(*c != JSB_KEY);
		c += sz;
	}
	return _jsb_type(c, 0) ? c - (uint8_t *)base : 0;
}

JSB_API size_t jsb_obj_get(const void *base, size_t offset, const size_t *meta, const void *key, size_t len){
	size_t sz;
	uint8_t *c = offset + (uint8_t *)base;
	if(JSB_OBJ != *c++)
		return 0;
	if(len == (size_t)-1)
		len = strsz(key);
	while(1){
		sz = _jsb_size(c, 0, meta);
		if(!sz) return 0;
		assert(*c == JSB_KEY);
		if(sz == len + 1 && mcmp(key, c+1, len) == 0)
			break;
		c += sz;
		sz = _jsb_size(c, 0, meta);
		if(!sz) return 0;
		assert(*c != JSB_KEY);
		c += sz;
	}
	c += sz;
	return _jsb_type(c, 0) ? c - (uint8_t *)base : 0;
}

/* return position of first slot who's record sorts >= than provided key, or n if it could just be appended */
PRIVATE size_t match_find(const size_t *keylens, const size_t *indexes, const void **keys, size_t n, const void *key, size_t keylen, int mode){
	size_t i, lo = 0, hi = n, mid, ret = n;
	mode = !!mode;
	while(lo < hi){
		mid = (lo >> 1) + (hi >> 1) + (lo & hi & 1);
		i = indexes[mid];
		if(i == n) goto hi;
		assert(i < n);
		/* sort by keylen, shortest to longest */
		if(keylens[i] > keylen) goto hi;
		if(keylens[i] < keylen) goto lo;
		/* keylens are equal - sort by key content, low to high */
		if(mcmp(keys[i], key, keylen) < mode) goto lo;
hi:
		ret = hi = mid;
		continue;
lo:
		ret = lo = mid + 1;
	}
	assert(ret <= n);
debug(("slot %zu\n", ret));
	return ret;
}

JSB_API void jsb_prepare(size_t *keyinfo, const void *_keys, uint32_t flags){
	const size_t n = keyinfo[0];
	const void **keys = (void *)_keys;
	size_t * const keylens = keyinfo + 1;
	size_t * const indexes = keylens + n;
	size_t i, j, k;
	for(i = 0; i < n; i++){
		assert(keys[i]);
		if((flags & JSB_STRLEN) || keylens[i] == (size_t)-1)
			keylens[i] = strsz(keys[i]);
		j = match_find(keylens, indexes, keys, i, keys[i], keylens[i], 1);
		assert(i <= n);
		for(k = i; k > j; k--)
			indexes[k] = indexes[k-1];
		indexes[j] = i;
	}

#if CHECK
	for(i = 0, j = 1; j < n; i++, j++){
		assert(keylens[indexes[i]] <= keylens[indexes[j]]);
		if(keylens[indexes[i]] == keylens[indexes[j]])
			switch(mcmp(keys[indexes[i]], keys[indexes[j]], keylens[indexes[i]])){
				default: assert(0);
				case -1: break;
				case  0: assert(indexes[i] < indexes[j]);
			}
	}
#endif

}

JSB_API size_t jsb_match(const void *base, size_t offset, const size_t *meta, const void *_keys, const size_t *keyinfo, size_t *offsets){
	const size_t n = keyinfo[0];
	const void **keys = (void *)_keys;
	size_t i, j, ret = 0, len, val;
	const uint8_t * const bin = base;
	const size_t * const keylens = keyinfo + 1;
	const size_t * const indexes = keylens + n;

	/* enter the json object */
	if(JSB_OBJ != bin[offset])
		goto error;
	offset++;

	/* loop through matches, zero out offset */
	for(i = 0; i < n; i++)
		offsets[i] = 0;

again:
	/* iterate through its key/value pairs - examine first token */
	len = _jsb_size(base, offset, meta);
	if(len){
		assert(bin[offset] == JSB_KEY);
		/* grab value offset, and bump key/len to skip over the leading token */
		val = offset++ + len--;

		/* find first possible match slot for current key */
		i = match_find(keylens, indexes, keys, n, bin + offset, len, 0);
		/* scan for an unused slot match */
		while(i < n){
			/* map slot to key index */
			j = indexes[i++];
			if(keylens[j] == len && mcmp(keys[j], bin + offset, len) == 0){
				if(offsets[j])
					continue;
				offsets[j] = val;
				if(++ret == n)
					goto done;
			}
			break;
		}
		len = _jsb_size(base, val, meta);
		assert(len);
		if(!len) goto error;
		/* seek to next key */
		offset = val + len;
		goto again;
	}else{
		assert(bin[offset] == JSB_OBJ_END);
	}
done:
	return ret;
error:
	ret = JSB_ERROR;
	goto done;
}

typedef struct {
	const uint8_t *msd;
	const uint8_t *dot;
	const uint8_t *ipart;
	const uint8_t *ipart_end;
	const uint8_t *fpart;
	const uint8_t *fpart_end;
	const uint8_t *epart;
	const uint8_t *epart_end;
	unsigned negative:1;
	unsigned e_negative:1;
	unsigned zero:1;
} ns_t;

/* harvest boundaries of parts of scientific-notation-style numbers */
PRIVATE void numwalk(const uint8_t *s, ns_t *ns){
	ns->ipart = ns->ipart_end = 0;
	ns->fpart = ns->fpart_end = 0;
	ns->epart = ns->epart_end = 0;
	ns->negative = (*s == '-');
	ns->e_negative = 0;
	ns->zero = 1;

	s += (*s == '-' || *s == '+');
	ns->ipart = ns->msd = s;
	while(*s >= '0' && *s <= '9'){
		if(ns->zero && '0' != *s){
			ns->msd = s;
			ns->zero = 0;
		}
		s++;
	}
	ns->ipart_end = ns->dot = s;
	if(*s == '.'){
		ns->fpart = ++s;
		while(*s >= '0' && *s <= '9'){
			if(ns->zero && '0' != *s){
				ns->msd = s;
				ns->zero = 0;
			}
			s++;
		}
		ns->fpart_end = s;
	}
	if(*s == 'e' || *s == 'E'){
		ns->e_negative = (*++s == '-');
		s += (*s == '-' || *s == '+');
		ns->epart = s;
		while(*s >= '0' && *s <= '9')
			s++;
		ns->epart_end = s;
	}
}

PRIVATE const uint8_t *step(int *r, ssize_t *d, int m, const uint8_t *ea, const uint8_t *eb){
	/* convert next least significant digit to numeric, else zero if we've run out */
	int x = (ea != eb) ? (*--eb - '0') * m : 0;
	int y = *d % 10; /* grab last delta digit: [-9 .. 9]               */
	*d /= 10;        /* shift delta right one digit                    */
	x += y;          /* merge                                          */
	*r = x % 10;     /* return right-most digit                        */
	*d += x / 10;    /* shift right, and merge with delta              */
	return eb;       /* return possibly changed end-of-exponent marker */
}

PRIVATE const uint8_t *istep(int *r, const uint8_t *c, ns_t *ns){
	if(c == ns->ipart_end)   /* jump from end of integer to fractional */
		c = ns->fpart;       /* fpart may equal fpart_end              */
	if(c == ns->fpart_end)   /* pad with trailing zero if at end       */
		*r = '0';
	else
		*r = *c++;
	return c;
}

/* numerically compare two stringified json-style numbers */
PRIVATE int numcmp(const uint8_t *n0, const uint8_t *n1){
	ns_t ns0, ns1;
	ssize_t d0, d1;
	int r = 0, m0, m1, r0, r1;
	const uint8_t *c0, *c1;

	/* find boundaries in scientific notation strings */
	numwalk(n0, &ns0);
	numwalk(n1, &ns1);

	/* first check if either evaluate to zero */
	if(ns0.zero && ns1.zero) return 0;
	if(ns0.zero) return ns1.negative ?  1 : -1;
	if(ns1.zero) return ns0.negative ? -1 :  1;

	/* else both are non-zero - check the sign for an easy win */
	if( ns0.negative && !ns1.negative) return -1;
	if(!ns0.negative &&  ns1.negative) return  1;
	/* signs are the same - treat as positive, and invert later as needed */

	/* calculate exponent normalization delta */
	d0 = ns0.dot - ns0.msd + (ns0.msd > ns0.dot);
	d1 = ns1.dot - ns1.msd + (ns1.msd > ns1.dot);

	/* get end-of-exponent cursor */
	c0 = ns0.epart_end;
	c1 = ns1.epart_end;

	/* convert negative exponent into multiplier */
	m0 = 1 - (ns0.e_negative << 1);
	m1 = 1 - (ns1.e_negative << 1);

	/* now step through both exponents to compare, adding the delta on the fly */
	while(d0 || d1 || c0 != ns0.epart || c1 != ns1.epart){
		/* returns next least significant digit of result in r0/r1 */
		c0 = step(&r0, &d0, m0, ns0.epart, c0);
		c1 = step(&r1, &d1, m1, ns1.epart, c1);
		/* if r0 == r1, preserve existing compare value */
		r = r0 < r1 ? -1 : r0 > r1 ? 1 : r;
	}

	/* larger or smaller exponent wins */
	if(!r){
		/* step through digits starting at msd, padding w/ zeros as necessary */
		c0 = ns0.msd;
		c1 = ns1.msd;
		do{
			c0 = istep(&r0, c0, &ns0);
			c1 = istep(&r1, c1, &ns1);
			r = r0 - r1;
			r = (r > 0) - (r < 0);
		}while(!r && (c0 != ns0.fpart_end || c1 != ns1.fpart_end));
	}

	/* invert result if both were negative */
	if(ns0.negative /* && ns1.negative */)
		r = -r;

	return r;
}

JSB_API int jsb_cmp(const void *base0, size_t offset0, const void *base1, size_t offset1){
	const uint8_t *n0 = (const uint8_t *)base0 + offset0;
	const uint8_t *n1 = (const uint8_t *)base1 + offset1;
	int t;

	switch(*n0){
		default:        return 0;
		case JSB_NULL:  return *n1 == JSB_NULL;
		case JSB_FALSE: return (*n1 == JSB_TRUE)  ? -1 : (*n1 == JSB_FALSE);
		case JSB_TRUE:  return (*n1 == JSB_FALSE) ?  3 : (*n1 == JSB_TRUE);
		case JSB_STR:
		case JSB_KEY:
			if(*n1 != JSB_STR || *n1 != JSB_KEY)
				return 0;
			do{
				t = (*++n0 >= 0xf5) - (*++n1 >= 0xf5);
				if(t) break;
				t = *n0 - *n1;
				t = (t > 0) - (t < 0);
			}while(!t);
			return (t<<1)|1;
		case JSB_NUM:
			if(*n1 != JSB_NUM)
				return 0;
			return (numcmp(++n0, ++n1) << 1) | 1;
	}
}

/* ensure that all previously used jsb->state values actually fit */
STATIC_ASSERT(__COUNTER__ - CB <= 1<<(sizeof(((jsb_t *)NULL)->state)*CHAR_BIT), "ctr is too big");

#if JSB_PUBLIC
#if defined(_WIN32)
#include<windef.h>

BOOL APIENTRY DllMainCRTStartup(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved){
	(void)hinstDLL;
	(void)fdwReason;
	(void)lpvReserved;
	return TRUE;
}

#endif
#endif
