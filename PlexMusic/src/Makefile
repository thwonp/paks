###########################################################
# Supported platforms
SUPPORTED_PLATFORMS = tg5040
DEFAULT_PLATFORM = tg5040

# Set platform from environment or default
ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
PLATFORM=$(DEFAULT_PLATFORM)
endif

# Validate platform
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error Invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif

# Validate cross-compiler
ifeq (,$(CROSS_COMPILE))
$(error Missing CROSS_COMPILE for this toolchain)
endif

###########################################################

TARGET = plexmusic
PRODUCT = ../bin/$(PLATFORM)/$(TARGET).elf

# Common headers path (NextUI workspace)
NEXTUI_COMMON = ../../../../NextUI/workspace/all/common
NEXTUI_PLATFORM = ../../../../NextUI/workspace/$(PLATFORM)/platform

INCDIR  = -I.
INCDIR += -I./audio
INCDIR += -I./include
INCDIR += -I./include/mbedtls_lib
INCDIR += -I./include/parson
INCDIR += -I./include/fdk-aac
INCDIR += -I./include/libogg/include
INCDIR += -I./include/libopus/include
INCDIR += -I./include/opusfile/include
INCDIR += -I$(NEXTUI_COMMON)
INCDIR += -I$(NEXTUI_PLATFORM)
INCDIR += -I../../../../NextUI/workspace/$(PLATFORM)/libmsettings
INCDIR += -I../../../../NextUI/workspace/all/minarch/libretro-common/include

CC = $(CROSS_COMPILE)gcc

MY_CFLAGS  = -O2 -fomit-frame-pointer -std=gnu99
MY_CFLAGS += -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>'
MY_CFLAGS += -DOPUS_BUILD -DHAVE_LRINTF -DOP_DISABLE_HTTP -DOP_DISABLE_FLOAT_API
MY_CFLAGS += $(INCDIR)
MY_CFLAGS += -DPLATFORM=\"$(PLATFORM)\"
MY_CFLAGS += -DUSE_SDL2 -DUSE_GLES -DGL_GLEXT_PROTOTYPES
MY_CFLAGS += -I$(PREFIX)/include -I$(PREFIX_LOCAL)/include
# Force-include defines.h so BTN_ID_COUNT and friends are available to all TUs
MY_CFLAGS += -include $(NEXTUI_COMMON)/defines.h

MY_LDFLAGS  = -L$(PREFIX)/lib -L$(PREFIX_LOCAL)/lib
MY_LDFLAGS += -L$(PREFIX)/lib/$(CROSS_TRIPLE)
MY_LDFLAGS += -L../../../../NextUI/workspace/$(PLATFORM)/libmsettings
MY_LDFLAGS += $(shell pkg-config --libs sdl2 glesv2 2>/dev/null || echo "-lSDL2 -lGLESv2")
MY_LDFLAGS += -lSDL2_image -lSDL2_ttf
MY_LDFLAGS += -lmsettings -lasound -lfdk-aac -lsamplerate
MY_LDFLAGS += -lzip -lpthread -ldl -lz -lm

# mbedTLS source files (compiled from source for HTTPS support)
MBEDTLS_SRC = $(wildcard include/mbedtls_lib/*.c)

# Opus/OGG library sources (compiled separately to avoid header conflicts)
LIBOGG_SRC = include/libogg/src/bitwise.c include/libogg/src/framing.c
LIBOPUS_SRC = $(filter-out %/opus_custom_demo.c %/opus_demo.c %/opus_compare.c %/qext_compare.c %/repacketizer_demo.c %/mini_kfft.c, \
              $(wildcard include/libopus/src/*.c) \
              $(wildcard include/libopus/celt/*.c) \
              $(wildcard include/libopus/silk/*.c) \
              $(wildcard include/libopus/silk/float/*.c))
OPUSFILE_SRC = include/opusfile/src/info.c include/opusfile/src/internal.c \
               include/opusfile/src/opusfile.c include/opusfile/src/stream.c
OPUS_ALL_SRC = $(LIBOGG_SRC) $(LIBOPUS_SRC) $(OPUSFILE_SRC)

# Opus-specific include paths and flags (isolated to avoid API.h conflicts with project)
OPUS_INCDIR = -I./include/libogg/include -I./include/libopus/include -I./include/opusfile/include \
              -I./include/libopus -I./include/libopus/src -I./include/libopus/celt \
              -I./include/libopus/silk -I./include/libopus/silk/float
OPUS_CFLAGS = -O2 -fomit-frame-pointer -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF \
              -DOP_DISABLE_HTTP -DOP_DISABLE_FLOAT_API -std=gnu99

# NextUI common source files
NEXTUI_COMMON_SRC = \
    $(NEXTUI_COMMON)/utils.c \
    $(NEXTUI_COMMON)/api.c \
    $(NEXTUI_COMMON)/config.c \
    $(NEXTUI_COMMON)/scaler.c

NEXTUI_PLATFORM_SRC = $(NEXTUI_PLATFORM)/platform.c

# Main source files (explicit list to avoid picking up stub .c files for modules
# we intentionally exclude, and to control link order)
SOURCE = main.c \
         plex_log.c \
         module_auth.c \
         module_browse.c \
         module_player.c \
         module_settings.c \
         plex_queue.c \
         plex_api.c \
         plex_art.c \
         plex_auth.c \
         plex_config.c \
         plex_downloads.c \
         plex_favorites.c \
         plex_net.c \
         player.c \
         background.c \
         spectrum.c \
         radio.c \
         album_art.c \
         settings.c \
         ui_fonts.c \
         ui_icons.c \
         ui_utils.c \
         module_common.c \
         audio/kiss_fft.c \
         audio/kiss_fftr.c \
         include/parson/parson.c \
         include/mbedtls_entropy_alt.c \
         $(MBEDTLS_SRC) \
         $(NEXTUI_COMMON_SRC) \
         $(NEXTUI_PLATFORM_SRC)

OPUS_OBJ_DIR = opus_obj

all:
	@mkdir -p ../bin/$(PLATFORM) $(OPUS_OBJ_DIR)
	$(CC) -c $(OPUS_ALL_SRC) $(OPUS_CFLAGS) $(OPUS_INCDIR)
	@mv *.o $(OPUS_OBJ_DIR)/
	$(CC) $(SOURCE) $(OPUS_OBJ_DIR)/*.o -o $(PRODUCT) $(MY_CFLAGS) $(MY_LDFLAGS)
	@rm -rf $(OPUS_OBJ_DIR)

clean:
	rm -f ../bin/tg5040/$(TARGET).elf
	rm -rf $(OPUS_OBJ_DIR)
	rm -f *.o
