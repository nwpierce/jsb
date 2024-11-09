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
#define JSB_EOF       1 /* indicate the input buffer will not receive more data */
#define JSB_REVERSE   2 /* input binary, output json                            */
#define JSB_ASCII     4 /* when emitting json, escape all codepoints above 0x7f */
#define JSB_LINES     8 /* parse sequences of documents in either direction     */

/* flag bits for jsb_prepare() */
#define JSB_STRLEN    1

/* return code constants */
#define JSB_OK        0
#define JSB_DONE      1
#define JSB_ERROR     ((size_t)-1)

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

#ifndef JSB_DEFAULT_STACK_BYTES
#define JSB_DEFAULT_STACK_BYTES 8
#endif

#define JSB_DEFAULT_DEPTH (JSB_DEFAULT_STACK_BYTES * CHAR_BIT)

#define JSB_SIZE (sizeof(jsb_t) - JSB_DEFAULT_STACK_BYTES)

typedef struct {
	const uint8_t *next_in;
	size_t        avail_in;
	uint64_t      total_in;

	uint8_t       *next_out;
	size_t        avail_out;
	uint64_t      total_out;

	/* current json parser depth */
	size_t depth;

	/* maximum json parser depth */
	const size_t maxdepth;

	/* remaining fields are not useful to clients */
	const uint32_t flags;
	uint32_t code;
	uint8_t state;
	uint8_t key;
	uint8_t obj;
	uint8_t misc;
	uint8_t ch;
	uint8_t outb;
	uint8_t stack[JSB_DEFAULT_STACK_BYTES];
} jsb_t;

/* jsb_units() reports dynamic jsb size as a multiple of these */
typedef union {
	uint64_t u64;
	size_t sz;
	void *ptr;
} jsb_unit_t;


/**
 * Simple API
 **/

/* convert JSON to binary (or binary to JSON if JSB_REVERSE is set in flags)
 * return:
 *  output byte length or JSB_ERROR
 * notes:
 *  when emitting JSON, appends null byte to output, but does not include it in the returned size
 *  pass maxdepth=(size_t)-1 to request default maxdepth (64)
 */
JSB_API size_t jsb(void *dst, size_t dstlen, const void *src, size_t srclen, uint32_t flags, size_t maxdepth);


/**
 * Streaming API
 **/

/* return:
 *  number of jsb_unit_t's required to hold a jsb_t for desired maximum depth
 * note:
 *  pass maxdepth=(size_t)-1 to request default maxdepth (64)
 *  example using dynamically sized arrays:
 *   jsb_unit_t ju[jsb_units(1024)];
 *   jsb_t *jsb = (jsb_t)ju;
 *   size_t maxdepth = jsb_init(jsb, 0, sizeof(ju));
 *  or via an allocator:
 *   size_t jsz = jsb_units(1024) * sizeof(jsb_unit_t);
 *   jsb_t *jsb = alloca(jsz);
 *   jsb_init(jsb, 0, jsz);
 */
JSB_API size_t __attribute__((const)) jsb_units(size_t maxdepth);

/* initialize parser state, except for next_in/avail_in/next_out/avail_out
 * return:
 *  maximum supported object depth
 * flags that may be bitwise OR'd:
 *  JSB_REVERSE
 *  JSB_ASCII
 *  JSB_LINES
 *  JSB_EOF
 * note:
 *  pass jsbsize < JSB_SIZE (recommend: 0) to indicate default jsb stack bytes
 */
JSB_API size_t jsb_init(jsb_t *jsb, uint32_t flags, size_t jsbsize);

/* convert JSON input to binary output, or vice versa
 * return:
 *  JSB_OK:    parser is fine
 *  JSB_ERROR: something has gone awry
 * note:
 *  on JSB_OK, caller should check if input/output fields need updating
 *  duplicate keys are preserved
 *  key order is preserved
 *  for JSON => binary:
 *   JSON structure, UTF-8 encoding, and surrogate pair sequences are strictly enforced
 *  but for binary => JSON:
 *   input is not aggressively checked and can generate invalid json
 */
JSB_API size_t jsb_update(jsb_t *jsb);

/* call to indicate no additional bytes will be provided as input
 */
JSB_API void jsb_eof(jsb_t *jsb);


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
JSB_API void jsb_prepare(size_t *keyinfo, const void *keys, uint32_t flags);

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
