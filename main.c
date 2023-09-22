#undef NDEBUG

#define _FILE_OFFSET_BITS 64
#define _DEFAULT_SOURCE 1

#include<assert.h>
#include<errno.h>
#include<getopt.h>
#include<inttypes.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/ioctl.h>
#include<sys/mman.h>
#include<sys/stat.h>
#include<time.h>
#include<unistd.h>

#include"jsb.h"

typedef struct {
	int fd, state;
	const void *data;
	const size_t size;
	size_t len;
	uint64_t off, max;
} block_t;

static size_t pgsz(void){
	static size_t ps = 0;
	long z;
	if(!ps){
		z = sysconf(_SC_PAGESIZE);
		ps = z < 1 ? 65536 : z;
	}
	return ps;
}

static void *block_init(block_t *bk, int fd, size_t size, int stream){
	size_t ps = pgsz();
	void *data = NULL;
	off_t max = 0;
	size_t mod = size % ps;
	if(mod)
		size += ps - mod;
	else if(!size)
		size = ps;

	if(!stream){
		data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
		if(MAP_FAILED == data){
			data = NULL;
			stream = 1;
		}
	}
	if(!stream){
		max = lseek(fd, 0, SEEK_END);
		if(max < 1)
			stream = 1;
	}
	if(lseek(fd, 0, SEEK_SET))
		stream = 1;

	if(stream)
		data = mmap(data, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|(data ? MAP_FIXED : 0), -1, 0);

	bk->fd = fd;
	bk->state = MAP_FAILED == data ? 0 : (stream ? 1 : 2);
	*(void **)&bk->data = data;
	*(size_t *)&bk->size = size;
	bk->len = 0;
	bk->off = 0;
	bk->max = max;
	return data;
}

static void block_fini(block_t *bk){
	if(bk->data != MAP_FAILED){
		munmap((void *)bk->data, bk->size);
		*(void **)&bk->data = MAP_FAILED;
	}
}

static size_t block_next(block_t *bk){
	size_t r = bk->size;
	void *m;
	switch(bk->state){
		case 1: /* streaming input - just read into mmapped buffer */
			while(1){
				ssize_t sr = read(bk->fd, (void *)bk->data, r);
				if(sr < 0){
					if(EINTR == errno || EAGAIN == errno)
						continue;
					r = 0;
				}else{
					r = sr;
				}
				break;
			}
			break;
		case 2: /* initial mmap'd entrypoint */
			/* already loaded first block - just return it */
			if(r > bk->max){
				r = bk->max;
				bk->state = 0;
			}else{
				bk->state = 3;
			}
			break;
		case 3: /* subsequent mmap block - step on existing map */
			bk->off += r;
			m = mmap((void *)bk->data, r, PROT_READ, MAP_SHARED|MAP_FIXED, bk->fd, bk->off);
			if(m != bk->data){
				bk->state = 0;
				r = 0;
			}else if(bk->off + r >= bk->max){
				r = bk->max - bk->off;
				bk->state = 0;
			}
			break;
		default:
			r = 0;
	}
	return r;
}

static int fdwrite(int fd, const void *buf, size_t len){
	while(len){
		ssize_t w = write(fd, buf, len);
		switch(w){
			case -1:
				if(EAGAIN == errno)
					continue;
				/* fall through */
			case 0:
				return -1;
			default:
				assert(w > 0);
		}
		buf = w + (uint8_t *)buf;
		len -= w;
	}
	return 0;
}

static void usage(int fd){
	static char u[] =
		"Usage: jsb [options] < input > output\n"
		"\n"
		"Options:\n"
		"	-v  verify input only (no output)\n"
		"	-s  force streaming input (disable mmap)\n"
		"	-r  input window size (bytes, default 32mb)\n"
		"	-w  output window size (bytes, default 32mb)\n"
		"	-l  process concatenated json / binary records\n"
		"	-a  force ascii output for binary -> json\n"
		"	-t  log timing information to stderr\n"
		"	-h  this help\n";
	fdwrite(fd, u, sizeof(u) - 1);
	exit(1);
}

int main(int argc, char **argv){
	int r, ch, ret = 1;
	jsb_t jsb;
	uint8_t *dst = NULL;
	size_t dstlen = 0, rlen = 0, wlen = 0, doc = 0, rv;
	size_t total = 0;
	ssize_t len;
	int ifd = fileno(stdin);
	int ofd = fileno(stdout);
	int ufd = fileno(stderr);
	uint8_t *src;
	int emit = 1, stream = 0, timeit = 0;
	uint32_t flags = 0;
	block_t bk;
	clock_t t0, t1;
	do{
		switch(ch = getopt(argc, argv, "hsvaltr:w:")){
			case -1:  break;
			case 's': stream = 1; break;
			case 'v': emit = 0; break;
			case 'w': wlen = strtoul(optarg, NULL, 0); break;
			case 'r': rlen = strtoul(optarg, NULL, 0); break;
			case 'l': flags |= JSB_LINES; break;
			case 'a': flags |= JSB_ASCII; break;
			case 't': timeit = 1; break;
			case 'h': ufd = ofd; /* fall through */
			default:  usage(ufd); break;
		}
	}while(ch != -1);

	if(!rlen)
		rlen = 1<<25;
	if(!wlen)
		wlen = 1<<25;

	dst = malloc(dstlen = wlen);
	assert(dst);

	t0 = clock();

	src = block_init(&bk, ifd, rlen, stream);
	assert(MAP_FAILED != src);

	len = block_next(&bk);

	switch(*src){
		case JSB_NULL:
		case JSB_TRUE:
		case JSB_FALSE:
		case JSB_NUM:
		case JSB_STR:
		case JSB_OBJ:
		case JSB_ARR:
			flags |= JSB_REVERSE;
			/* fall through */
		default:
			break;
		case 0xc0:
		case 0xc1:
		case JSB_OBJ_END:
		case JSB_ARR_END:
		case JSB_DOC_END:
			goto done;
	}

	jsb_init(&jsb, dstlen, flags);
	jsb_srclen(&jsb, len);
	total += len;

again:
	rv = jsb_update(&jsb, dst, src);
	switch(rv){
		default:
			if(!(flags & JSB_LINES) && doc)
				goto done;
			doc++;
			goto again;
		case JSB_FILL:
			len = block_next(&bk);
			jsb_srclen(&jsb, len);
			total += len;
			goto again;
		case JSB_FULL:
			len = jsb_consume(&jsb, dst, -1);
			if(emit){
				r = fdwrite(ofd, dst, len);
				assert(0 == r);
			}
			goto again;
		case JSB_ERROR:
			goto done;
		case JSB_EOF:
			break;
	}
	len = jsb_consume(&jsb, dst, -1);
	if(emit){
		r = fdwrite(ofd, dst, len);
		assert(0 == r);
		if((flags & JSB_REVERSE) && !(flags & JSB_LINES)){
			r = fdwrite(ofd, "\n", 1);
			assert(0 == r);
		}
	}
	ret = close(ofd);
	t1 = clock();
	if(timeit)
		fprintf(stderr, "%.3f mb/s\n", (CLOCKS_PER_SEC / 1048576.0) * total / (double)(t1 - t0));
done:
	block_fini(&bk);
	free(dst);
	return ret;
}
