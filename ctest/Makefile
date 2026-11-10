# The standalone C suite for zukomp's core (#66): no R headers, no R.
#
#   make -C ctest          build and run zkt_ctest
#   make -C ctest strict   from clean, with warnings as errors
#   make -C ctest asan     ASan + UBSan, in its own build directory
#
# It links the same sources the package compiles, under the same miniz, zstd
# and lz4 defines as src/Makevars and fuzz/build.sh, so it tests the build
# users run. If it stops building with an Rinternals.h error, R has leaked
# into src/zu_internal.h (CLAUDE.md, Fuzzing).
#
# One object per source, each from a compile command of its own, with header
# dependencies (-MMD -MP). Mutation testing needs exactly this: Mull
# re-parses every file from its recorded command line, and one command that
# compiles several files cannot be re-parsed (r-actions mutation.yml). The
# compiler and flags are stamped, and every object is rebuilt when they
# change: mutation.yaml passes Mull's CC, and objects left from a plain build
# would give it an uninstrumented binary that reports no mutants, silently.
#
# GNU make is fine here, unlike src/Makevars: ctest/ is not in the package.

CC     ?= cc
SRC    := ../src
VEND   := $(SRC)/vendor
BUILD  ?= build
BIN    ?= zkt_ctest
SAN    ?=

INC    := -I$(SRC) -I../inst/include -I$(VEND)/miniz -I$(VEND)/zstd -I$(VEND)/lz4 -I.

# src/Makevars' trim set. MINIZ_NO_ASSERT as well: the package sets it.
MINIZ_DEFS := -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ARCHIVE_WRITING_APIS \
              -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
              -DMINIZ_NO_PNG_APIS -DMINIZ_NO_ASSERT
ZSTD_DEFS  := -DXXH_NAMESPACE=ZSTD_ -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 \
              -DZSTDLIB_VISIBLE= -DZSTDERRORLIB_VISIBLE=
LZ4_DEFS   := -DXXH_NAMESPACE=LZ4_ -DLZ4LIB_VISIBILITY= -DLZ4FLIB_VISIBILITY= \
              -DNDEBUG

# -Wno-missing-field-initializers: a vtable written before level_fast and
# level_best existed leaves them out on purpose; zero is "not advertised".
WARN   := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
          -Wold-style-definition -Wvla -Wno-missing-field-initializers
CFLAGS ?= -O1 -g
OWNFLAGS := -std=c99 $(CFLAGS) $(SAN) $(WARN) $(INC) $(MINIZ_DEFS) $(ZSTD_DEFS)
# Vendored code keeps its own dialect and warning policy (CLAUDE.md, Vendored
# native code): its warnings are upstream's.
VFLAGS   := $(CFLAGS) $(SAN) -w $(INC)

# The pure-C core: src/*.c minus the R glue (init.c, zu_whole.c,
# zukomp_r.c, zukomp_api.c, zukomp_test.c), the same list as fuzz/build.sh.
CORE   := zu_status zu_registry zu_buf zu_stream codec_identity \
          codec_deflate zu_gzip zu_frames codec_zstd codec_lz4
TESTS  := test_main test_api test_roundtrip test_fixtures test_limits test_gzip

ZSTD_SRC := $(wildcard $(VEND)/zstd/common/*.c $(VEND)/zstd/compress/*.c \
                       $(VEND)/zstd/decompress/*.c)
LZ4_SRC  := $(wildcard $(VEND)/lz4/*.c)

OWNOBJ  := $(patsubst %,$(BUILD)/%.o,$(CORE) $(TESTS))
# zstd's sources are in three directories: the object names keep the
# directory, so two files of one name cannot collide.
ZSTD_OBJ := $(foreach s,$(ZSTD_SRC),$(BUILD)/v_zstd_$(notdir $(patsubst %/,%,$(dir $(s))))_$(notdir $(s:.c=.o)))
VENDOBJ  := $(BUILD)/v_miniz.o $(ZSTD_OBJ) \
            $(patsubst %.c,$(BUILD)/v_lz4_%.o,$(notdir $(LZ4_SRC)))

.PHONY: all run strict asan clean
all: run

# Rewritten, and so every object rebuilt, whenever the compiler or flags
# change. Compared when the Makefile is read, so it sees CC, CFLAGS and SAN
# from the command line.
STAMP    := $(BUILD)/.flags
FLAGSNOW := $(CC) $(OWNFLAGS) $(VFLAGS)
ifneq ($(FLAGSNOW),$(shell cat $(STAMP) 2>/dev/null))
$(shell mkdir -p $(BUILD) && printf '%s\n' '$(FLAGSNOW)' > $(STAMP))
endif
$(STAMP):
	mkdir -p $(BUILD)
	printf '%s\n' '$(FLAGSNOW)' > $@

$(BUILD)/%.o: $(SRC)/%.c $(STAMP)
	$(CC) $(OWNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.c $(STAMP)
	$(CC) $(OWNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/v_miniz.o: $(VEND)/miniz/miniz.c $(STAMP)
	$(CC) $(VFLAGS) $(MINIZ_DEFS) -MMD -MP -c -o $@ $<

define zstd_rule
$(BUILD)/v_zstd_$(notdir $(patsubst %/,%,$(dir $(1))))_$(notdir $(1:.c=.o)): $(1) $(STAMP)
	$$(CC) $$(VFLAGS) $$(ZSTD_DEFS) -MMD -MP -c -o $$@ $$<
endef
$(foreach s,$(ZSTD_SRC),$(eval $(call zstd_rule,$(s))))

$(BUILD)/v_lz4_%.o: $(VEND)/lz4/%.c $(STAMP)
	$(CC) $(VFLAGS) $(LZ4_DEFS) -MMD -MP -c -o $@ $<

-include $(wildcard $(BUILD)/*.d)

$(BIN): $(OWNOBJ) $(VENDOBJ)
	$(CC) $(CFLAGS) $(SAN) -o $@ $(OWNOBJ) $(VENDOBJ)

run: $(BIN)
	./$(BIN)

strict: clean
	$(MAKE) WARN='$(WARN) -Werror' run

# -fno-sanitize-recover: a UBSan report fails the run instead of scrolling
# past. detect_leaks=0 on macOS, where LeakSanitizer is unsupported.
asan:
	$(MAKE) BUILD=build-asan BIN=zkt_ctest_asan \
	  SAN='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined' \
	  zkt_ctest_asan
	ASAN_OPTIONS=detect_leaks=$(if $(filter Darwin,$(shell uname -s)),0,1):abort_on_error=1 \
	  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./zkt_ctest_asan

clean:
	rm -rf build build-asan zkt_ctest zkt_ctest_asan *.dSYM
