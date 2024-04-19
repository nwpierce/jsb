#ifndef _JSB_H
#define _JSB_H

/* use this to tweak function attributes if desired */
#ifndef JSB_API
#define JSB_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#include<limits.h>
#include<stdint.h>
#include<stddef.h>

#ifndef JSB_API
#define JSB_API
#endif

/* flag bits for jsb() and jsb_init() */
#define JSB_REVERSE   1 /* input binary, output json                            */
#define JSB_ASCII     2 /* when emitting json, escape all codepoints above 0x7f */
#define JSB_LINES     4 /* parse sequences of documents in either direction     */
#define JSB_SIZE      8 /* causes jsb_init() to return jsb_t size in size_t's   */

/* flag bits for jsb_prepare() */
#define JSB_STRLEN    1

/* return code constants */
#define JSB_ERROR     (SIZE_MAX - 0)
#define JSB_FILL      (SIZE_MAX - 1)
#define JSB_FULL      (SIZE_MAX - 2)
#define JSB_EOF       (SIZE_MAX - 3)

/* largest valid size */
#define JSB_SIZE_MAX  (SIZE_MAX - 4)

/* binary value type markers */
#define JSB_OBJ        0xf5
#define JSB_ARR        0xf6
#define JSB_NULL       0xf7
#define JSB_FALSE      0xf8
#define JSB_TRUE       0xf9
#define JSB_NUM        0xfa
#define JSB_STR        0xfb
#define JSB_KEY        0xfc

/* additional binary markers */
#define JSB_OBJ_END    0xfd
#define JSB_ARR_END    0xfe
#define JSB_DOC_END    0xff

/* stringified constants for the above markers */
#define JSB_OBJ_S     "\xf5"
#define JSB_ARR_S     "\xf6"
#define JSB_NULL_S    "\xf7"
#define JSB_FALSE_S   "\xf8"
#define JSB_TRUE_S    "\xf9"
#define JSB_NUM_S     "\xfa"
#define JSB_STR_S     "\xfb"
#define JSB_KEY_S     "\xfc"
#define JSB_OBJ_END_S "\xfd"
#define JSB_ARR_END_S "\xfe"
#define JSB_DOC_END_S "\xff"

typedef struct {
	size_t dstpos;
	size_t srcpos;
	size_t dstlen;
	size_t srclen;
	size_t stack;
	size_t depth;
	size_t flags;
	size_t eat;
	uint32_t code;
	uint8_t state;
	uint8_t misc;
	uint8_t ch;
	uint8_t bt;
} jsb_t;


/**
 * Simple API
 **/

/* convert JSON to binary (or binary to JSON if JSB_REVERSE is set in flags)
 * returns output byte length or JSB_ERROR
 * when emitting JSON, appends null byte to output, but does not include it in the returned size
 */
JSB_API size_t jsb(void *dst, size_t dstlen, const void *src, size_t srclen, size_t flags);


/**
 * Streaming API
 **/

/* if JSB_SIZE is set in flags, return number of size_t's required to hold jsb_t
 * else initialize parser state with destination buffer size and return 0
 */
JSB_API size_t jsb_init(jsb_t *jsb, size_t dstlen, size_t flags);

/* convert JSON input to binary output, or vice versa
 * return:
 *  JSB_ERROR: something has gone awry
 *  JSB_FILL:  call jsb_srclen() to indicate EOF or supply binary
 *  JSB_FULL:  call jsb_expand() and/or jsb_consume() to make space in dst
 *  JSB_EOF:   parser cleanly reached end of input stream
 *  <n>:       parsing current document completed, caller may consume/process first n bytes from dst
 * once JSB_FILL/JSB_FULL are dealt with, call jsb_update() again
 * note:
 *  n will be less than JSB_EOF
 *  duplicate keys are preserved
 *  key order is preserved
 * for JSON => binary:
 *  JSON structure, UTF-8 encoding, and surrogate pair sequences are strictly enforced
 * but for binary => JSON:
 *  input is not aggressively checked and can generate invalid json
 */
JSB_API size_t jsb_update(jsb_t *jsb, void *dst, const void *src);

/* declare queued input byte length, use srclen=0 to indicate EOF
 * return 0 on succcess or JSB_ERROR if previous input still remains
 * note:
 *  only call when current source length is zero
 *  (after jsb_init() or when jsb_update() returns JSB_FILL)
 */
JSB_API size_t jsb_srclen(jsb_t *jsb, size_t srclen);

/* supply current dst buffer and desired new size
 * return actual new size (which may be larger if requested size was too small)
 * note:
 *  to shrink, call jsb_dstlen(), then realloc() (or equivalent)
 *  to expand, call realloc(), then jsb_dstlen()
 */
JSB_API size_t jsb_dstlen(jsb_t *jsb, void *dst, size_t dstlen);

/* declare caller will consume up to the first n bytes from start of
 * dst buffer before calling additional methods against this jsb
 * return requested count or less, if fewer bytes are available
 * note:
 *  most efficient if caller is willing to consume all available data by passing:
 *    n = (size_t)-1
 */
JSB_API size_t jsb_consume(jsb_t *jsb, void *dst, size_t n);


/**
 * Binary traversal API
 **/

/* These are built for speed and do not perform bounds checking nor aggressively
 * defend against invalid input. Badly structured input binary can cause malformed
 * output json or cause out-of-bounds reads.
 */

/* return one of the above (non-zero) value type markers [0xf5 .. 0xfc]
 * or zero on failure
 */
JSB_API uint8_t jsb_type(const void *base, size_t offset);

/* examines the json value at provided offset
 * returns:
 *  1 for true, non-empty strings/keys/arrays/objects, and non-zero numbers
 *  0 for false, null, empty strings/keys/arrays/objects, and numbers that equal zero
 *  JSB_ERROR for errors (bad base/offset)
 */
JSB_API size_t jsb_bool(const void *base, size_t offset);

/* using up to n size_t's in meta
 *  walk the binary at base + offset
 *  collect size/count for the largest items >= m bytes
 *  index records by start offset
 * returns number of items covered (at most, n/3-1)
 * note:
 *  remaining functions will use a non-NULL meta to look up value sizes and array/object item counts
 */
JSB_API size_t jsb_analyze(const void *base, size_t offset, size_t *meta, size_t n, size_t m);

/* return number of bytes backing value or zero on error
 * optionally pass meta as filled by jsb_analyze() (or NULL)
 * note:
 *  required terminating sentinel byte >= 0xf5 is not included
 *  UTF-8 contents of strings/keys/numbers is size-1 bytes at 1+(uint8_t *)value
 */
JSB_API size_t jsb_size(const void *base, size_t offset, const size_t *meta);

/* count number of keys in object, values in array, or codepoints in a string or number
 * optionally pass meta as filled by jsb_analyze() (or NULL)
 * return count as described, zero for true/false/null, or JSB_ERROR on error
 */
JSB_API size_t jsb_count(const void *base, size_t offset, const size_t *meta);

/* scans object to fetch offset of first value associated with provided key
 * optionally pass meta as filled by jsb_analyze() (or NULL)
 * return 0 on failure
 * pass len = -1 to call strlen() internally
 */
JSB_API size_t jsb_obj_get(const void *base, size_t offset, const size_t *meta, const void *key, size_t len);

/* walks array to fetch offset of value at requested array index
 * optionally pass meta as filled by jsb_analyze() (or NULL)
 * return 0 on failure
 */
JSB_API size_t jsb_arr_get(const void *base, size_t offset, const size_t *meta, size_t idx);

/* for matching against json objects - client should:
 * a) construct an array of n*2+1 size_t's, containing:
 *  1) number of keys (n) in the first slot
 *  2) either:
 *    *) key lengths (in bytes) in the next n slots (use -1 per key to internally strlen)
 *    *) or set JSB_STRLEN bit in flags to internally strlen all keys
 * b) construct an array n corresponding pointers to raw UTF-8 key strings (no preceeding JSB_KEY token, no JSON escapes)
 * c) call jsb_prepare to fill in the remaining n slots so it may be passed to jsb_match()
 * note:
 *  you may specify the same key multiple times to look for duplicates
 */
JSB_API void jsb_prepare(size_t *keyinfo, const void *keys, size_t flags);

/* scan json object at supplied offset to harvest value offsets for a list of keys
 * optionally pass meta as filled by jsb_analyze() (or NULL)
 * keys pointer contents should be equivalent to that passed to jsb_prepare()
 * keyinfo pointer contents should be equivalent to that passed to jsb_prepare()
 * offsets array should have room for an offset per key
 * returns number of keys matched
 *  matched offsets will be (non-zero) offsets of associated key values
 *  non-matched offsets will be zeroed
 *  match order is scan order - in the event of duplicates, first key/value pair is favored
 *  if the same key is specified multiple times, subsequent value offsets will be collected
 */
JSB_API size_t jsb_match(const void *base, size_t offset, const size_t *meta, const void *keys, const size_t *keyinfo, size_t *offsets);

/* compare two scalar json values of matching class
 * returns:
 *     0: error (arrays, objects, mismatched value class, or bad type code)
 *    -1: less than
 *     1: equal to
 *     3: greater than
 *   if non-zero: (r >> 1) => -1/0/1, corresponding to lt/eq/gt, memcmp-style
 * note - will only return non-errors for:
 *   null vs null
 *   boolean vs boolean
 *   string/key vs string/key
 *   number vs number
 * in order to support arbitrary precision, numbers are manually compared,
 * rather than potentially lossily parsed into native floating point types
 */
JSB_API int jsb_cmp(const void *base0, size_t offset0, const void *base1, size_t offset1);

#ifdef __cplusplus
}
#endif

#endif
