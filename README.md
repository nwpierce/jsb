# libjsb

Libjsb streams JSON to/from a friendly compact traversable binary representation with minimal storage overhead. Intended to support easy and efficient consumption and production of JSON. Motivation and/or inspiration for libjsb include [jsmn](https://github.com/zserge/jsmn) and Andrew Kelley's talk on [Data-Oriented Design](https://vimeo.com/649009599).

## Features:

* [fast](#benchmarks)
* incremental
* embeddable
	* no runtime dependencies
	* no internal memory allocations
* no imposed maximum nested object/array depth
	* borrows tail end of provided output buffer to track state (one byte per level of depth)
* maintains object key order
* strict input JSON checking
	* restricts UTF-8 codepoints to ranges:
		* 0x0 - 0xd7ff
		* 0xe000 - 0x10ffff
	* requires codepoints to be encoded in shortest possible sequences
	* enforces proper surrogate pair sequences
* numbers stored internally as strings
	* no imposed loss of precision
	* scientific notation exponents:
		* '+' is stripped
		* 'E' lowercased to 'e'
* reasonably compact [binary representation](#binary-representation)
	* at most, two bytes larger than input JSON
* can optionally process multiple concatenated JSON documents
* can optionally emit pure ASCII JSON
* resulting binary form can be traversed
	* may optionally be indexed to accelerate traversal routines

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

Linux - 3.3GHz i3-2120:

| compiler   | json -> binary | binary -> json |
|------------|----------------|----------------|
| gcc 7.4.0  | 485 mb/sec     | 575 mb/sec     |
| gcc 8.3.0  | 430 mb/sec     | 565 mb/sec     |
| gcc 9.3.0  | 445 mb/sec     | 410 mb/sec     |
| gcc 10.2.1 | 475 mb/sec     | 405 mb/sec     |
| gcc 11.3.0 | 420 mb/sec     | 525 mb/sec     |
| gcc 12.2.0 | 420 mb/sec     | 555 mb/sec     |

Linux - 1.2 (3.8 Turbo boost) GHz i7-1060NG7 (via qemu):

| compiler   | json -> binary | binary -> json |
|------------|----------------|----------------|
| gcc 7.4.0  | 780 mb/sec     | 935 mb/sec     |
| gcc 8.3.0  | 685 mb/sec     | 895 mb/sec     |
| gcc 9.3.0  | 760 mb/sec     | 610 mb/sec     |
| gcc 10.2.0 | 765 mb/sec     | 580 mb/sec     |
| gcc 11.3.0 | 645 mb/sec     | 630 mb/sec     |
| gcc 12.2.0 | 705 mb/sec     | 825 mb/sec     |

macOS - 1.2 (3.8 Turbo boost) GHz i7-1060NG7:

| compiler   | json -> binary | binary -> json |
|------------|----------------|----------------|
| clang 15   | 570 mb/sec     | 485 mb/sec     |

Linux - Apple M1 (via qemu):

| compiler   | json -> binary | binary -> json |
|------------|----------------|----------------|
| gcc 7.4.0  | 985 mb/sec     | 1145 mb/sec    |
| gcc 8.3.0  | 1030 mb/sec    | 955 mb/sec     |
| gcc 9.3.0  | 835 mb/sec     | 1000 mb/sec    |
| gcc 10.2.1 | 880 mb/sec     | 1015 mb/sec    |
| gcc 11.3.0 | 985 mb/sec     | 1135 mb/sec    |
| gcc 12.2.0 | 970 mb/sec     | 1105 mb/sec    |

macOS - Apple M1:

| compiler   | json -> binary | binary -> json |
|------------|----------------|----------------|
| clang 15   | 750 mb/sec     | 950 mb/sec     |

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
