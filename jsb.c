#include"jsb.h"

#define   likely(x) x
#define unlikely(x) x
#ifdef __has_builtin
#if __has_builtin(__builtin_expect)
#undef    likely
#undef  unlikely
#define   likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#endif

#if RELEASE
#undef NDEBUG
#define NDEBUG
#undef DEBUG
#else
#include<string.h>
#endif

#ifdef NDEBUG
#define assert(cond) ((void)0)
#else
#include<assert.h>
#include<stdarg.h>
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

/* the _jsb_load() and _jsb_dump() functions are designed as coroutines
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

/* copy potentially overlapping buffer from end to start */
static void mrcp(uint8_t *dst, uint8_t const *src, size_t len){
	const uint8_t *end = dst;
	dst += len;
	src += len;
	while(dst != end)
		*--dst = *--src;
}

/* copy potentially overlapping buffer from start to end */
static void mfcp(uint8_t *dst, uint8_t const *src, size_t len){
	const uint8_t *end = dst + len;
	while(dst != end)
		*dst++ = *src++;
}

/* memcpy() */
static void mcp(void * __restrict dst, void const * __restrict src, size_t len){
	mfcp(dst, src, len);
}

/* memmove() */
static void mmv(void *dst, void *src, size_t len){
	uintptr_t d = (uintptr_t)dst;
	uintptr_t s = (uintptr_t)src;
	assert((uintptr_t)-1 - d > len);
	assert((uintptr_t)-1 - s > len);
	if(d == s)
		return;
	if(d < s){
		if(d + len > s){
			mfcp(dst, src, len);
			return;
		}
	}else{
		if(s + len > d){
			mrcp(dst, src, len);
			return;
		}
	}
	mcp(dst, src, len);
}

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
	size_t ret;
	assert(s);
	while(*c)
		c++;
	ret = c - (const uint8_t *)s;
#if !RELEASE
	assert(strlen(s) == ret);
#endif
	return ret;
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
static uint8_t space(uint8_t ch){
	if(likely(ch > ' ')) return 0;
	return likely(ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
}

/* digit */
static uint8_t digit(uint8_t ch){
	return ch >= '0' && ch <= '9';
}

/* positive digit */
static uint8_t pdigit(uint8_t ch){
	return ch >= '1' && ch <= '9';
}

/* map hexidecimal character to numeric value or -1 on error */
static int hex(uint8_t ch){
	if(ch >= '0' && ch <= '9')
		return ch - '0';
	if(ch >= 'a' && ch <= 'f')
		return ch - ('a' - 0xa);
	if(ch >= 'A' && ch <= 'F')
		return ch - ('A' - 0xa);
	return -1;
}

/* map lower 4 bits to hexidecimal character */
static uint8_t nibble(uint8_t ch){
	ch &= 0xf;
	return ch + (ch < 0xa ? '0' : 'a' - 0xa);
}

/* generic heap routines */

typedef int heap_test(void *data, size_t a, size_t b);
typedef void heap_swap(void *data, size_t a, size_t b);

static void heap_siftdown(void *data, size_t n, heap_test test, heap_swap swap, size_t i){
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

static void heap_ify(void *data, size_t n, heap_test test, heap_swap swap){
	size_t i = n / 2;
	while(i--)
		heap_siftdown(data, n, test, swap, i);
}

static void heap_sort(void *data, size_t n, heap_test test, heap_swap swap){
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

static void swap_nodes(void *data, size_t a, size_t b){
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
static int sz_a_gt_b(node_t a, node_t b){
	assert(a != b);
	return a[1] > b[1] || (a[1] == b[1] && a[2] > b[2]);
}

/* wrapper for the above */
static int hs_sz_a_gt_b(void *data, size_t a, size_t b){
	node_t *nodes = data;
	return sz_a_gt_b(nodes[a], nodes[b]);
}

/* used for second pass - maxheap by offset */
static int hs_off_a_lt_b(void *data, size_t a, size_t b){
	node_t *nodes = data;
	assert(a != b);
	return nodes[a][0] < nodes[b][0];
}

static void idx_insert(size_t n, size_t *idx, node_t v){
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

static size_t idx_finish(size_t *idx, size_t n){
	size_t i = (n < idx[0]) ? (idx[0] = n) : (n = idx[0]);
	node_t *arr = (node_t *)(idx + IDX_HDR);
	/* now heapsort on byte-offset column, low -> high */
	heap_sort(arr, i, hs_off_a_lt_b, swap_nodes);
	/* and cap it with an out-of-range offset */
	arr[i][0] = SIZE_MAX;
	return n;
}

/* return first index node >= supplied offset, possibly off=SIZE_MAX */
static const size_t *idx_seek(const size_t *idx, size_t off){
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

static const size_t *idx_find(const size_t *idx, size_t off){
	const size_t *r = NULL;
	if(idx && idx[0]){
		r = idx_seek(idx, off);
		if(r[0] != off)
			r = NULL;
	}
	return r;
}

static size_t _jsb_str_count(const uint8_t *bin, size_t off, size_t *endpos){
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

static size_t _jsb_size(const void *base, size_t offset, const size_t *meta);

static size_t idx_load(const uint8_t *bin, size_t off, size_t *idx, const size_t n, const size_t m, size_t d){
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

#define JSZ (sizeof(jsb_t) / sizeof(size_t) + !!(sizeof(jsb_t) % sizeof(size_t)))

/* initialize parse object - provide size of output/state buffer */
static  size_t _jsb_init(jsb_t *jsb, size_t dstlen, size_t flags);
JSB_API size_t  jsb_init(jsb_t *jsb, size_t dstlen, size_t flags){ return _jsb_init(jsb, dstlen, flags); }
static  size_t _jsb_init(jsb_t *jsb, size_t dstlen, size_t flags){
	size_t r = (flags & JSB_SIZE) ? JSZ : 0;
	debug(("jsb_init(%p, %zu, %zx)\n", (void *)jsb, dstlen, flags));
	if(!r){
		jsb->dstpos = jsb->srcpos = jsb->srclen = 0;
		jsb->depth = jsb->eat = 0;
		jsb->code = 0;
		jsb->ch = jsb->bt = jsb->state = jsb->misc = 0;
		jsb->dstlen = jsb->stack = dstlen;
		jsb->flags = flags;
	}
	return r;
}

/* call at start of streaming api functions to complete jsb_consume */
static void _jsb_eat(jsb_t *jsb, uint8_t *dst){
	size_t len, eat = jsb->eat, dstpos = jsb->dstpos;
	if(eat){
		assert(eat <= dstpos);
		len = dstpos - eat;
		mmv(dst, dst + eat, len);
		jsb->dstpos = len;
		jsb->eat = 0;
	}
}

/* indicate caller will consume up to n bytes from dst */
JSB_API size_t jsb_consume(jsb_t *jsb, void *dst, size_t n){
	debug(("jsb_consume(%p, %p, %zu)\n", (void *)jsb, dst, n));
	_jsb_eat(jsb, dst);
	if(n < jsb->dstpos){
		debug(("eat: %zu < %zu\n", n, jsb->dstpos));
		jsb->eat = n;
	}else{
		debug(("eat: %zu -> %zu\n", n, jsb->dstpos));
		n = jsb->dstpos;
		jsb->dstpos = 0;
	}
	return n;
}

/* call after growing or prior to shrinking dst buffer */
JSB_API size_t jsb_dstlen(jsb_t *jsb, void *dst, size_t dstlen){
	size_t stklen, min, stk;
	debug(("jsb_dstlen(%p, %p, %zu)\n", (void *)jsb, dst, dstlen));
	_jsb_eat(jsb, dst);
	/* grab current stack size at end of dst buffer */
	stklen = jsb->dstlen - jsb->stack;
	/* round requested dstlen up to minimum buffer size */
	min = jsb->dstpos + stklen;
	if(dstlen < min)
		dstlen = min;
	/* calculate new stack offset */
	stk = dstlen - stklen;
	/* relocate the (possibly overlapping) stack contents */
	mmv((uint8_t *)dst + stk, (uint8_t *)dst + jsb->stack, stklen);
	/* fill in new stack offset */
	jsb->stack = stk;
	/* and set & return new dst buffer size */
	return jsb->dstlen = dstlen;
}

/* call to supply parser with new input chunk size */
static  size_t _jsb_srclen(jsb_t *jsb, size_t len);
JSB_API size_t  jsb_srclen(jsb_t *jsb, size_t len){ return _jsb_srclen(jsb, len); }
static  size_t _jsb_srclen(jsb_t *jsb, size_t len){
	debug(("jsb_srclen(%p, %zu)\n", (void *)jsb, len));
	if(jsb->srcpos != jsb->srclen || JSB_INT_EOF == jsb->ch){
		assert(0);
		return JSB_ERROR;
	}
	if(len){
		jsb->srcpos = 0;
		jsb->srclen = len;
	}else{
		jsb->ch = JSB_INT_EOF;
	}
	return 0;
}

#define F(x) f_ ## x
#define J(x) j_ ## x

#define JUMP(target) GOTO(J(target))

/* see: https://stackoverflow.com/questions/36932774/reset-counter-macro-to-zero */
enum { CB = __COUNTER__ };

#define YIELD(sz) do{                          \
	enum { ctr = __COUNTER__ - CB };           \
	jsb->state = ctr;                          \
	ret = sz;                                  \
	GOTO(F(yield));                            \
	case ctr: break;                           \
}while(0)

#define UNSTASH (assert(stack < jsb->dstlen),dst[stack++])

#define BEGIN(x) switch(x){ default: ERROR; case 0

#define END }

#define ADDCH do{                              \
	debug(("addch: %02x\n", ch));              \
	assert(JSB_INT_EOF != ch);                 \
	while(dstpos == stack)                     \
		YIELD(JSB_FULL);                       \
	dst[dstpos++] = ch;                        \
}while(0)

#define APPEND(x) do{                          \
	bt = (x);                                  \
	debug(("append: %02x\n", bt));             \
	assert(JSB_INT_EOF != bt);                 \
	while(dstpos == stack)                     \
		YIELD(JSB_FULL);                       \
	dst[dstpos++] = bt;                        \
}while(0)

#define STASH(x) do{                           \
	bt = (x);                                  \
	debug(("stash: %02x\n", bt));              \
	while(dstpos == stack)                     \
		YIELD(JSB_FULL);                       \
	dst[--stack] = bt;                         \
}while(0)

#define NEXT(sw) do{                           \
	debug(("next!\n"));                        \
	while(1){                                  \
		if(srcpos != srclen){                  \
			ch = src[srcpos++];                \
			if(0xc0 == ch || 0xc1 == ch)       \
				ERROR;                         \
			if(sw && space(ch))                \
				continue;                      \
			debug(("ch: %02x\n", ch));         \
			break;                             \
		}else if(JSB_INT_EOF == ch){           \
			debug(("ch: EOF\n"));              \
			break;                             \
		}else{                                 \
			YIELD(JSB_FILL);                   \
		}                                      \
	}                                          \
}while(0)

#define ERROR do{ jsb->code = __LINE__; JUMP(error); }while(0)

static size_t _jsb_load(jsb_t *jsb, uint8_t * const dst, const uint8_t * const src){
	size_t ret;

	/* clone, but no need to restore read-only ptr */
	const size_t srclen = jsb->srclen;

	size_t srcpos, stack, dstpos;
	uint8_t key, obj, shift, ch, bt;
	uint32_t tmp;

	debug(("jsb_load(%p, %p, %p)\n", (void *)jsb, dst, src));
	_jsb_eat(jsb, dst);


	/* clone on entry & restore on yield */
	dstpos = jsb->dstpos;
	srcpos = jsb->srcpos;
	stack  = jsb->stack;
	key    = ((1<<3) & jsb->misc) >> 3;
	obj    = ((1<<2) & jsb->misc) >> 2;
	shift  = ((3<<0) & jsb->misc) >> 0;
	ch = jsb->ch;
	bt = jsb->bt;

	debug(("enter: %d\n", jsb->state));

BEGIN(jsb->state):
	JUMP(value);

J(escape):
	NEXT(0);
	if('u' == ch){
		jsb->code = 0;
		shift = 3;
		JUMP(hex);
	}
	else if('t' == ch) ch = '\t';
	else if('n' == ch) ch = '\n';
	else if('r' == ch) ch = '\r';
	else if('f' == ch) ch = '\f';
	else if('b' == ch) ch = '\b';
	else if('"' != ch && '/' != ch && '\\' != ch)
		ERROR;
	ADDCH;
	JUMP(string2);

J(endnum):
	if(JSB_INT_EOF != ch){
		debug(("srcpos--\n"));
		srcpos--;
	}
	JUMP(more);

J(more):
	if(!jsb->depth)
		JUMP(done);
	debug(("more: obj/key = %u/%u\n", obj, key));
	NEXT(1);
	if(",:"[key] == ch){
		if(key ^= obj)
			JUMP(key);
		JUMP(value);
	}else if("]}"[obj] == ch){
		if(key)
			ERROR;
		JUMP(pop);
	}
	ERROR;

J(key):
	NEXT(1);
J(key2):
	if(ch != '"')
		ERROR;
	APPEND(JSB_KEY);
	if(0)
J(string):
		APPEND(JSB_STR);
J(string2):
	NEXT(0);
	if(JSB_INT_EOF == ch)
		ERROR;
	if('"' == ch){
		JUMP(more);
	}else if('\\' == ch){
		JUMP(escape);
	}else if(ch >= 0x20 && ch < 0x80){
		ADDCH;
	}else if((ch & 0xe0) == 0xc0 && (ch & 0x1f)){
		jsb->code = ch & 0x1f;
		shift = 1;
		JUMP(unicode);
	}else if((ch & 0xf0) == 0xe0){
		jsb->code = ch & 0xf;
		shift = 2;
		JUMP(unicode);
	}else if((ch & 0xf8) == 0xf0){
		jsb->code = ch & 0x7;
		shift = 3;
		JUMP(unicode);
	}else ERROR;
	JUMP(string2);

J(value):
	debug(("value!\n"));
	NEXT(1);
	if(0)
J(value2):
		debug(("value2!\n"));
	if('"' == ch) JUMP(string);
	else if(pdigit(ch)) JUMP(number);
	else if('-' == ch) JUMP(sign);
	else if('0' == ch) JUMP(zero);
	else if('t' == ch) JUMP(true);
	else if('f' == ch) JUMP(false);
	else if('n' == ch) JUMP(null);
	else if('{' == ch) key = 1;
	else if('[' == ch) assert(!key);
	else ERROR;
	JUMP(push);

J(push):
	NEXT(1);
	APPEND(JSB_ARR - key); /* JSB_ARR - 1 == JSB_OBJ */
	if("]}"[key] == ch){
		APPEND(JSB_ARR_END - key); /* JSB_ARR_END - 1 == JSB_OBJ_END */
		key = 0;
		JUMP(more);
	}
	STASH(obj);
	obj = key;
	jsb->depth++;
	if(key)
		JUMP(key2);
	JUMP(value2);

J(pop):
	APPEND(JSB_ARR_END - obj); /* JSB_ARR_END - 1 == JSB_OBJ_END */
	obj = UNSTASH;
	assert(obj < 2);
	key = 0;
	jsb->depth--;
	JUMP(more);

J(sign):
	APPEND(JSB_NUM);
	ADDCH;
	NEXT(0);
	if(pdigit(ch))
		JUMP(number2);
	if('0' == ch)
		JUMP(zero2);
	ERROR;

J(number):
	APPEND(JSB_NUM);
J(number2):
	ADDCH;
	NEXT(0);
	if(digit(ch))
		JUMP(number2);
	if('.' == ch)
		JUMP(decimal);
	if('e' == ch || 'E' == ch)
		JUMP(exponent);
	JUMP(endnum);

J(zero):
	APPEND(JSB_NUM);
J(zero2):
	ADDCH;
	NEXT(0);
	if('.' == ch)
		JUMP(decimal);
	if('e' == ch || 'E' == ch)
		JUMP(exponent);
	JUMP(endnum);

J(decimal):
	ADDCH;
	NEXT(0);
	if(likely(digit(ch)))
		JUMP(decimal_more);
	ERROR;

J(decimal_more):
	ADDCH;
	NEXT(0);
	if(digit(ch))
		JUMP(decimal_more);
	if('e' == ch || 'E' == ch)
		JUMP(exponent);
	JUMP(endnum);

J(exponent):
	ch = 'e';
	ADDCH;
	NEXT(0);
	if('-' == ch || '+' == ch){
		if('-' == ch)
			ADDCH;
		NEXT(0);
	}
	if(digit(ch))
		JUMP(exponent_more);
	ERROR;

J(exponent_more):
	ADDCH;
	NEXT(0);
	if(digit(ch))
		JUMP(exponent_more);
	JUMP(endnum);

J(null):
	jsb->code = 'u' | ('l'<<7) | ('l'<<14);
	bt = JSB_NULL;
	JUMP(const);

J(false):
	jsb->code =	'a' | ('l'<<7) | ('s'<<14) | ('e'<<21);
	bt = JSB_FALSE;
	JUMP(const);

J(true):
	jsb->code = 'r' | ('u'<<7) | ('e'<<14);
	bt = JSB_TRUE;
	JUMP(const);

J(const):
	do{
		NEXT(0);
		if((jsb->code & 0x7f) != ch) ERROR;
	}while(jsb->code >>= 7);
	APPEND(bt);
	JUMP(more);

J(unicode):
	ADDCH;
	NEXT(0);
	if((ch & 0xc0) != 0x80)
		ERROR;
	jsb->code = (jsb->code << 6) | (ch & 0x3f);
	if(--shift)
		JUMP(unicode);
	if(jsb->code > 0x10ffff || (jsb->code >= 0xd800 && jsb->code < 0xe000))
		ERROR;
	ADDCH;
	JUMP(string2);

J(hexb):
	NEXT(0);
	if('\\' != ch)
		ERROR;
	NEXT(0);
	if('u' != ch)
		ERROR;
	jsb->code <<= 16;
	shift = 3;
	JUMP(hex);

J(hex):
	NEXT(0);
	tmp = hex(ch);
	if(tmp > 0xf)
		ERROR;
	jsb->code |= tmp << (4 * shift);
	if(shift){
		shift--;
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
	if(jsb->code < 0x80){
		ch = jsb->code;
		ADDCH;
	}else if(jsb->code < (0x1<<11)){
		ch = 0xc0 | (jsb->code >> 6);
		ADDCH;
		ch = 0x80 | (jsb->code & 0x3f);
		ADDCH;
	}else if(jsb->code < (0x1<<16)){
		ch = 0xe0 | (jsb->code >> 12);
		ADDCH;
		ch = 0x80 | ((jsb->code >> 6) & 0x3f);
		ADDCH;
		ch = 0x80 | (jsb->code & 0x3f);
		ADDCH;
	}else if(jsb->code < (0x11<<16)){
		ch = 0xf0 | (jsb->code >> 18);
		ADDCH;
		ch = 0x80 | ((jsb->code >> 12) & 0x3f);
		ADDCH;
		ch = 0x80 | ((jsb->code >> 6) & 0x3f);
		ADDCH;
		ch = 0x80 | (jsb->code & 0x3f);
		ADDCH;
	}else ERROR;
	JUMP(string2);

J(error): /* parsing failed */
	debug(("error!\n"));
	while(1)
		YIELD(JSB_ERROR);
J(done):
	APPEND(JSB_DOC_END);
	YIELD(dstpos);
	NEXT(1);
	if(JSB_INT_EOF == ch)
		while(1)
			YIELD(JSB_EOF);
	if(jsb->flags & JSB_LINES)
		JUMP(value2);
	ERROR;
F(yield): /* save state and suspend */
	debug(("yield: %d\n", ret));
	jsb->bt = bt;
	jsb->ch = ch;
	assert(obj   < 2);
	assert(key   < 2);
	assert(shift < 4);
	jsb->misc = (key << 3) | (obj << 2) | (shift << 0);
	jsb->stack = stack;
	jsb->srcpos = srcpos;
	jsb->dstpos = dstpos;
	return ret;
END;
}

#undef NEXT
#define NEXT do{                             \
	debug(("next!\n"));                      \
	assert(JSB_INT_EOF != ch);               \
	do{                                      \
		if(srcpos != srclen){                \
			ch = src[srcpos++];              \
			if(0xc0 == ch || 0xc1 == ch)     \
				ERROR;                       \
			debug(("ch: %02x\n", ch));       \
			break;                           \
		}else if(JSB_INT_EOF != ch){         \
			YIELD(JSB_FILL);                 \
		}else{                               \
			break;                           \
		}                                    \
	}while(1);                               \
}while(0)

#undef ERROR
#define ERROR do{ code = __LINE__; JUMP(error); }while(0)

static size_t _jsb_dump(jsb_t *jsb, uint8_t * const dst, const uint8_t * const src){
	size_t ret;

	/* clone, but no need to restore read-only ptr */
	const size_t srclen = jsb->srclen;

	size_t dstpos, srcpos, depth, stack;
	uint32_t code;
	uint8_t pch, ch, bt;

	const uint32_t ascii = jsb->flags & JSB_ASCII;

	debug(("jsb_dump(%p, %p, %p)\n", (void *)jsb, dst, src));
	_jsb_eat(jsb, dst);

	/* clone on entry & restore on yield */
	dstpos = jsb->dstpos;
	srcpos = jsb->srcpos;
	depth = jsb->depth;
	stack = jsb->stack;
	code = jsb->code;
	pch = jsb->misc;
	ch = jsb->ch;
	bt = jsb->bt;

BEGIN(jsb->state):
	NEXT;
	JUMP(start);
	do{
		if(JSB_ARR_END == ch || JSB_OBJ_END == ch){
			pch = 0;
			bt = "]}"[ch&1];
			if(!depth--) ERROR;
			JUMP(append);
		}else if(JSB_KEY == pch){
			APPEND(':');
		}else if(JSB_ARR != pch && JSB_OBJ != pch){
			APPEND(',');
		}
J(start):
		code = 0;
		pch = ch;
		if(JSB_KEY == ch || JSB_STR == ch){
			APPEND('"');
			NEXT;
			while(likely(ch < 0xf5)){
				if(ch < 0x20 || ch == '"' || ch == '\\'){
					APPEND('\\');
					if('"' == ch || '\\' == ch) (void)ch;
					else if('\t' == ch) ch = 't';
					else if('\n' == ch) ch = 'n';
					else if('\r' == ch) ch = 'r';
					else if('\f' == ch) ch = 'f';
					else if('\b' == ch) ch = 'b';
					else{
						APPEND('u');
						APPEND('0');
						APPEND('0');
						APPEND(nibble(ch>>4));
						ch = nibble(ch);
					}
				}else if(ascii && (ch & 0x80)){
					if((ch & 0xf8) == 0xf0){
						code = ch & 0x7;
						goto _3;
					}
					if((ch & 0xf0) == 0xe0){
						code = ch & 0xf;
						goto _2;
					}
					assert((ch & 0xe0) == 0xc0);
					code = ch & 0x1f;
					goto _1;
					_3: NEXT; assert((ch & 0xc0) == 0x80); code <<= 6; code |= ch ^ 0x80;
					_2: NEXT; assert((ch & 0xc0) == 0x80); code <<= 6; code |= ch ^ 0x80;
					_1: NEXT; assert((ch & 0xc0) == 0x80); code <<= 6; code |= ch ^ 0x80;
					assert(code < 0x110000);
					APPEND('\\');
					APPEND('u');
					if(code < 0x10000){
						APPEND(nibble(code >> 12));
						APPEND(nibble(code >> 8));
						APPEND(nibble(code >> 4));
					}else{
						code -= 0x10000;
						APPEND('d');
						APPEND(nibble((code >> 18) | 0x8));
						APPEND(nibble(code >> 14));
						APPEND(nibble(code >> 10));
						APPEND('\\');
						APPEND('u');
						APPEND('d');
						APPEND(nibble((code >> 8) | 0xc));
						APPEND(nibble(code >> 4));
					}
					ch = nibble(code);
				}
				ADDCH;
				NEXT;
			}
			APPEND('"');
			continue;
		}else if(JSB_NUM == ch){
			NEXT;
			while(likely(ch < 0xf5)){
				ADDCH;
				NEXT;
			}
			continue;
		}else if((JSB_ARR == ch || JSB_OBJ == ch)){
			bt = "[{"[ch&1];
			if(!++depth) ERROR;
		}else if(JSB_TRUE == ch){
			bt = 't';
			code = 'r' | ('u'<<7) | ('e'<<14);
		}else if(JSB_FALSE == ch){
			bt = 'f';
			code = 'a' | ('l'<<7) | ('s'<<14) | ('e'<<21);
		}else if(JSB_NULL == ch){
			bt = 'n';
			code = 'u' | ('l'<<7) | ('l'<<14);
		}else{
			ERROR;
		}
J(append):
		APPEND(bt);
		while(code){
			APPEND(code & 0x7f);
			code >>= 7;
		}
		NEXT;
	}while(depth);
	/* parsing successful */
	if(jsb->flags & JSB_LINES)
		APPEND('\n');  /* append a newline */
	APPEND(0);       /* and null terminate */
	dstpos--; /* but don't include in output */
	debug(("done!\n"));
	YIELD(dstpos);
	if(jsb->flags & JSB_LINES){
again:
		NEXT;
		switch(ch){
			case JSB_DOC_END:
				goto again;
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
				break;
		}
	}
	while(1)
		YIELD(JSB_EOF);
J(error): /* parsing failed */
	debug(("error!\n"));
	while(1) YIELD(JSB_ERROR);
F(yield): /* save state and suspend */
	debug(("yield: %d\n", ret));
	jsb->bt = bt;
	jsb->ch = ch;
	jsb->misc = pch;
	jsb->code = code;
	jsb->stack = stack;
	jsb->depth = depth;
	jsb->srcpos = srcpos;
	jsb->dstpos = dstpos;
	return ret;
END;
}

static  size_t _jsb_update(jsb_t *jsb, void * const dst, const void * const src);
JSB_API size_t  jsb_update(jsb_t *jsb, void * const dst, const void * const src){ return _jsb_update(jsb, dst, src); }
static  size_t _jsb_update(jsb_t *jsb, void * const dst, const void * const src){
	return (jsb->flags & JSB_REVERSE) ? _jsb_dump(jsb, dst, src) : _jsb_load(jsb, dst, src);
}

JSB_API size_t jsb(void *dst, size_t dstlen, const void *src, size_t srclen, size_t flags){
	jsb_t jsb;
	size_t ret = 0, rv;
	size_t r;
	_jsb_init(&jsb, dstlen, flags & ~(size_t)JSB_SIZE);
	r = _jsb_srclen(&jsb, srclen);
	assert(0 == r);
	(void)r;
again:
	rv = _jsb_update(&jsb, dst, src);
	switch(rv){
		default:
			ret += rv;
			goto again;
		case JSB_FILL:
			r = _jsb_srclen(&jsb, 0);
			assert(0 == r);
			(void)r;
			goto again;
		case JSB_FULL:
		case JSB_ERROR:
			ret = JSB_ERROR;
			/* fall through */
		case JSB_EOF:
			break;
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

static  uint8_t _jsb_type(const void *base, size_t offset);
JSB_API uint8_t  jsb_type(const void *base, size_t offset){ return _jsb_type(base, offset); }
static  uint8_t _jsb_type(const void *base, size_t offset){
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
	while(t = bin[++offset], t < 0xf5){
		if(t == '0' || t == '.' || t == '-') continue;
		assert((t >= '1' && t <= '9') || t == 'e');
		return t != 'e';
	}
	return 0;
}

JSB_API size_t  jsb_size(const void *base, size_t offset, const size_t *meta){ return _jsb_size(base, offset, meta); }
static  size_t _jsb_size(const void *base, size_t offset, const size_t *meta){
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
static size_t match_find(const size_t *keylens, const size_t *indexes, const void **keys, size_t n, const void *key, size_t keylen, int mode){
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

JSB_API void jsb_prepare(size_t *keyinfo, const void *_keys, size_t flags){
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
