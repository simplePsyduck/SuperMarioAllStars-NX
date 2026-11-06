# Super Mario All-Stars - Nintendo Switch homebrew (.nro), devkitPro libnx + SDL2.
#
#   make            builds SuperMarioAllStars.nro
#   make clean
#
# Needs devkitPro (DEVKITPRO set) with: switch-dev switch-sdl2 switch-mesa switch-freetype switch-harfbuzz
# (dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-freetype switch-harfbuzz). See README.md.

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set: export DEVKITPRO=/opt/devkitpro")
endif

APP_TITLE   := Super Mario All-Stars
APP_AUTHOR  := Darhkfox, TeamReadTail, & simplePsyduck
APP_VERSION := 1.0.0
TARGET      := SuperMarioAllStars
BUILD       := build
ICON        := icon.jpg
ROMFS       := romfs

DEVKITA64 := $(DEVKITPRO)/devkitA64
LIBNX     := $(DEVKITPRO)/libnx
PORTLIBS  := $(DEVKITPRO)/portlibs/switch
TOOLS     := $(DEVKITPRO)/tools/bin
PREFIX    := $(DEVKITA64)/bin/aarch64-none-elf-
CXX       := $(PREFIX)g++

SRC := ../src

# the emulation core, the games' modules and the app: as on Windows and Android
SOURCES := \
  $(SRC)/core/cpu65816.cpp $(SRC)/core/apu.cpp $(SRC)/core/dsp.cpp $(SRC)/core/ppu.cpp \
  $(SRC)/core/snes.cpp $(SRC)/core/rewind.cpp $(SRC)/core/png.cpp $(SRC)/core/fileio.cpp \
  $(SRC)/core/disasm.cpp \
  $(SRC)/game/smas.cpp $(SRC)/game/smb1.cpp $(SRC)/game/smb1coop.cpp $(SRC)/game/smb2.cpp \
  $(SRC)/game/smb3.cpp $(SRC)/game/smb3coop.cpp $(SRC)/game/achievements.cpp $(SRC)/game/palette.cpp \
  $(SRC)/app/app.cpp $(SRC)/app/config.cpp $(SRC)/app/menu.cpp $(SRC)/app/launcher.cpp \
  $(SRC)/app/music.cpp $(SRC)/app/achstore.cpp $(SRC)/app/utf8.cpp $(SRC)/app/sys_posix.cpp \
  $(SRC)/platform/android/video_gles.cpp $(SRC)/platform/android/audio_sdl.cpp \
  $(SRC)/platform/android/touch.cpp \
  $(SRC)/platform/switch/app_switch.cpp $(SRC)/platform/switch/input_nx.cpp \
  $(SRC)/platform/switch/nx_sys.cpp $(SRC)/platform/switch/glyphs_nx.cpp

OBJS := $(patsubst $(SRC)/%.cpp,$(BUILD)/%.o,$(SOURCES))

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
INCLUDES := -I$(SRC) -I$(LIBNX)/include -I$(PORTLIBS)/include -I$(PORTLIBS)/include/SDL2 \
            -I$(PORTLIBS)/include/freetype2
CXXFLAGS := -g -Wall -O2 -ffunction-sections -Wno-unused-parameter -Wno-sign-compare -Wno-psabi \
            $(ARCH) -D__SWITCH__ -DSMAS_SWITCH $(INCLUDES) -std=gnu++17
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBDIRS  := -L$(LIBNX)/lib -L$(PORTLIBS)/lib
# (static libraries: the order matters)
LIBS     := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lfreetype -lharfbuzz -lbz2 -lpng16 -lz -lnx -lm

.PHONY: all clean
all: $(TARGET).nro

$(BUILD)/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo compiling $(notdir $<)
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	@echo linking $(notdir $@)
	@$(CXX) $(LDFLAGS) $(OBJS) $(LIBDIRS) $(LIBS) -o $@

$(BUILD)/$(TARGET).nacp:
	@mkdir -p $(BUILD)
	@$(TOOLS)/nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp $(ICON) $(wildcard $(ROMFS)/*)
	@$(TOOLS)/elf2nro $< $@ --icon=$(ICON) --nacp=$(BUILD)/$(TARGET).nacp --romfsdir=$(ROMFS)
	@echo built $@

clean:
	@rm -rf $(BUILD) $(TARGET).nro

-include $(OBJS:.o=.d)
