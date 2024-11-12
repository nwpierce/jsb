##
# command line tunables - override with: make foo=bar
##

# build mode - by default, squash asserts and disable aggressive debug logging
release=1
debug=0

# set to non-zero to clean/distclean prior to building any targets
clean=0
distclean=0

# optimization level, LTO, and C standard
opt=3
lto=0
std=c89


##
# misc
##

# use JSON Test Suite to check for proper behavior
jts=https://github.com/nst/JSONTestSuite.git

# things to delete for clean/distclean
clean_targets=jsb check test.results check.dSYM $(wildcard *.so *.dylib *.dll *.o)
distclean_targets=JSONTestSuite


##
# compiler/linker environment probes
##

# harvest the linker help text
linker:=$(shell $(LINK.c) -Wl,--help -x c < /dev/null 2>&1)
# and preprocessor macros
macros:=$(shell $(CC) -E -dM - < /dev/null)

# set up macos/windows tweaks
so=so
ifeq (__APPLE__,$(filter __APPLE__,$(macros)))
so=dylib
# Apple requires shared libs to be linked against System
libjsb.$(so):  LDLIBS+=-lSystem
# this lib has no initialization side effects
libjsb.$(so):  LDFLAGS+=-Wl,-mark_dead_strippable_dylib
else
ifeq (_WIN32,$(filter _WIN32,$(macros)))
so=dll
# export symbols for dll
jsb.o: CPPFLAGS+=-DJSB_API="__declspec(dllexport) __cdecl"
endif
endif


##
# build flags
##

# apply above C standard, optimization, and lto settings
CFLAGS=-Wall -Wextra -pedantic -std=$(std) -O$(opt) $(if $(filter-out 0,$(lto)),-flto)
LDFLAGS=-O$(opt) $(if $(filter-out 0,$(lto)),-flto)

ifneq (0,$(debug))
CPPFLAGS+=-DDEBUG
release=0
endif

ifneq (0,$(release))
CPPFLAGS+=-DRELEASE
ifneq (,$(filter --strip-all,$(linker)))
libjsb.$(so): LDFLAGS+=-Wl,--strip-all
endif
endif

jsb.o: CFLAGS+=-fPIC -fno-builtin

libjsb.$(so): CPPFLAGS+=-DJSB_PUBLIC
libjsb.$(so): LDFLAGS+=-shared -fPIC -fno-builtin -nostdlib

check: CPPFLAGS+=-DCHECK

main.o: CPPFLAGS=-D_GNU_SOURCE
main.o: CFLAGS+=-fPIC


##
# user targets
##

# default target
all: libjsb.$(so)

# git clones JSONTestSuite
depend: JSONTestSuite/README.md

# fake targets - handled below
clean distclean:
	@:

# shared lib
libjsb.$(so): jsb.o
	$(LINK.o) $< $(LDLIBS) -o $@

# run self tests and test against JSONTestSuite
test: check jsb Makefile depend
	./$<
	: local tests passed
	: testing against JSONTestSuite
	@$(MAKE) -j1 --no-print-directory subtest clean=0 > test.results
	: ensuring tests that should pass did...
	@! grep '^[^0].JSONTestSuite/test_parsing/y_' $<
	: ensuring tests that should not pass did not...
	@! grep '^[^1].JSONTestSuite/test_parsing/n_' $<
	: ensuring test results remain otherwise unchanged...
	@diff test.expect test.results
	: got expected results


##
# additional targets
##

# jsb object code
jsb.o: jsb.c jsb.h Makefile
	$(COMPILE.c) $< -o $@

# self tests
check: check.c jsb.c jsb.h Makefile
	$(filter-out -O%,$(filter-out -DRELEASE,$(LINK.c))) $(filter %.c,$^) -g -o $@
	./$@

# test frontend
jsb: main.o jsb.h Makefile libjsb.$(so)
	$(LINK.c) $(strip $(filter %.c,$^) $(filter %.o,$^)) -o $@ -Wl,-rpath,'$$ORIGIN' -L$$PWD -ljsb $(LDLIBS)

# clone JSONTestSuite on demand
JSONTestSuite/README.md:
	rm -rf jts JSONTestSuite
	git clone --depth 1 $(jts) jts
	mv jts JSONTestSuite

ifeq ($(MAKECMDGOALS),subtest)
TESTS=$(sort $(wildcard JSONTestSuite/test_*/*))

subtest: jsb JSONTestSuite/README.md Makefile $(addsuffix _test,$(TESTS))
	@echo >&2

%_test: %
	@./jsb -m 1024 -v < "$*" >/dev/null 2>/dev/null; printf "%d\t%s\n" $$? "$*"
	@printf . >&2
endif

# add clean/distclean as deps for object files if present in targets
%.o %.so: $(filter %clean,$(MAKECMDGOALS))
jsb check: $(filter %clean,$(MAKECMDGOALS))

# so that you can clean & build in one shot (try: make clean=1 <target>)
ifneq (,$(filter distclean,$(MAKECMDGOALS)))
distclean=1
endif
ifneq (,$(filter clean,$(MAKECMDGOALS)))
clean=1
endif

# handle clean/distclean deletions at makefile parse time
ifneq (,$(filter-out 0,$(clean) $(distclean)))
ifneq (0,$(distclean))
clean_targets+=$(distclean_targets)
endif
$(shell for x in $(wildcard $(clean_targets)); do rm -rf $$x && echo removed: $$x; done >&2)
endif

.PHONY: test subtest clean distclean check
.PRECIOUS: check
