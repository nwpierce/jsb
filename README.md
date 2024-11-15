# libjsb

Libjsb streams [JSON](https://www.json.org/) to/from a friendly traversable [binary representation](#binary-representation) with minimal storage overhead. Intended to support easy and efficient consumption and production of JSON.

## Features:

* [fast](#benchmarks)
* incremental
	* zlib-ish streaming interface
* embeddable
	* no external runtime dependencies
	* no internal heap memory allocations
* configurable maximum nested object/array depth
* maintains object key order
* strict input JSON checking
	* restricts UTF-8 codepoints to ranges:
		* 0x0 - 0xd7ff
		* 0xe000 - 0x10ffff
	* requires codepoints to be encoded in shortest possible sequences
	* enforces proper surrogate pair sequences
* strings stored as raw UTF-8 sequences
* numbers stored as strings
	* no imposed loss of precision
	* scientific notation exponents:
		* `+` is stripped
		* `E` lowercased to `e`
		* redundant leading zeros are stripped
		* sign is stripped if exponent is zero
	* includes an algorithm for numerically comparing stringified numbers
		* without internally parsing to native floating point (and potentially losing precision)
* reasonably compact [binary representation](#binary-representation)
	* at most, two bytes larger than input JSON, but usually smaller
* can optionally process multiple concatenated JSON documents
* can optionally emit pure ASCII JSON
* provides functions to traverse resulting binary
	* binary may be indexed to accelerate traversal routines

## Potentially less desirable features:

* no object key deduplication
* binary traversal and binary -> json routines do not aggressively defend against malformed input
	* only call those methods against well formed binary input (see below)
* bring your own numeric serialization routines

## Benchmarks

Using a large geojson file and doing:

```
make jsb
jsb < foo.geojson > foo.bin
```

Here are some results of repeatedly running both:
```
jsb -vt < foo.geojson
jsb -vt < foo.bin
```

In my limited experience, jsb's use of mmap is slow on Intel macOS. To reach higher throughput, add a `-s` to the flags to force it to use `read()`.


| OS    | CPU        | ghz     | compiler   | json -> binary | binary -> json |
|-------|------------|---------|------------|----------------|----------------|
| macOS | Apple M1   | 3.2     | clang 16.0 | 936 mb/sec     | 981 mb/sec     |
| macOS | i7-1060NG7 | 1.2/3.8 | clang 16.0 | 654 mb/sec     | 780 mb/sec     |
| Linux | i7-1060NG7 | 1.2/3.8 | gcc 12.2.0 | 650 mb/sec     | 852 mb/sec     |
| Linux | i7-1060NG7 | 1.2/3.8 | clang 16.0 | 667 mb/sec     | 832 mb/sec     |
| Linux | i3-2120    | 3.3     | gcc 12.2.0 | 423 mb/sec     | 495 mb/sec     |
| Linux | i3-2120    | 3.3     | clang-16.0 | 421 mb/sec     | 427 mb/sec     |

## Binary representation:

Design goals for the binary representation include:

* simple structure
* straightforward traversal
* strings, keys, and numbers stored as raw UTF-8 sequences
	* all JSON escapes decoded
	* surrogate pairs combined
* compact layout:
	* 1 byte per key/string/number (plus byte-length of raw UTF-8 string/number content)
	* 1 byte per true, false, or null
	* 2 bytes per object or array
	* 1 byte per document
* compared to input JSON:
	* null & true are three bytes smaller
	* false is four bytes smaller
	* strings are at least one byte smaller
	* numbers are one byte larger
	* arrays of length n are n-1 bytes smaller
	* objects with n key/value pairs are 2n-1 bytes smaller
	* documents are one byte larger

Valid JSON unicode character code points fall in ranges 0x0 - 0xd7ff and 0xe000 - 0x10FFFF. UTF-8-encoded sequences for these can involve all but 13 bytes, 11 of which are used as tokens in the binary representation:

* 0xc0: unused
* 0xc1: unused
* 0xf5: `JSB_NULL`
* 0xf6: `JSB_FALSE`
* 0xf7: `JSB_TRUE`
* 0xf8: `JSB_NUM`
* 0xf9: `JSB_STR`
* 0xfa: `JSB_KEY`
* 0xfb: `JSB_ARR`
* 0xfc: `JSB_OBJ`
* 0xfd: `JSB_ARR_END`
* 0xfe: `JSB_OBJ_END`
* 0xff: `JSB_DOC_END`

The binary format follows the following rules:

* All JSON values must be followed immediately by one of the above tokens, which is not included in the size reported by jsb_size()
* null, false, and true are represented by `JSB_NULL`, `JSB_FALSE`, and `JSB_TRUE`, respectively
* Numbers consist of `JSB_NUM`, followed by one or more UTF-8 characters representing a stringified number
* Strings consist of `JSB_STR`, followed by zero or more UTF-8 characters
* Keys consist of `JSB_KEY`, followed by zero or more UTF-8 characters
* Arrays consist of `JSB_ARR`, zero or more values, and `JSB_ARR_END`
* Objects consist of `JSB_OBJ`, zero or more key/value pairs, and `JSB_OBJ_END`
* Documents consist of a single JSON value followed by `JSB_DOC_END`

## API

See: [jsb.h](jsb.h)

## Build

Build just the shared library by running:

`make`

Run some tests via:

`make test`

Cross compile a presumably functional dll using a compiler from [musl.cc](https://musl.cc/):

`make CC=~/x86_64-w64-mingw32-cross/bin/x86_64-w64-mingw32-gcc`
