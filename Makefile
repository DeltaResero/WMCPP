# Wii Mandelbrot Computation Project Plus - Makefile
#---------------------------------------------------------------------------------
# Clear the implicit built-in rules
#---------------------------------------------------------------------------------
.SUFFIXES:

#---------------------------------------------------------------------------------
# Check if DEVKITPPC is set up correctly
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------

TARGET       :=  $(notdir $(CURDIR))
BUILD        :=  build
SOURCES      :=  src
DATA         :=
INCLUDES     :=
LIBOGC_INC   :=  $(DEVKITPRO)/libogc/include
LIBOGC_LIB   :=  $(DEVKITPRO)/libogc/lib/wii
PORTLIBS     :=  $(DEVKITPRO)/portlibs/wii

#---------------------------------------------------------------------------------
# Compiler and tools
#---------------------------------------------------------------------------------

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
STRIP = $(DEVKITPPC)/bin/powerpc-eabi-strip

#---------------------------------------------------------------------------------
# Options for code generation
#---------------------------------------------------------------------------------

CFLAGS      :=  -g -O2 -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS    :=  $(CFLAGS)

LDFLAGS     :=  -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# Any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS        :=  -lwiiuse -lbte -logc -lm -L$(PORTLIBS)/lib

#---------------------------------------------------------------------------------
# List of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS     :=  -L$(LIBOGC_LIB) -L$(PORTLIBS)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT    :=  $(CURDIR)/$(TARGET)

export VPATH     :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR   :=  $(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# Automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES        :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES        :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES        :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))

#---------------------------------------------------------------------------------
# Use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES    :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)

#---------------------------------------------------------------------------------
# Build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE   :=  $(foreach dir,$(INCLUDES), -iquote $(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                     -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC) -I$(PORTLIBS)/include

#---------------------------------------------------------------------------------
# Build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS  :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                     -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@make --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

#---------------------------------------------------------------------------------
run:
	wiiload $(TARGET).dol

#---------------------------------------------------------------------------------
else

DEPENDS :=  $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# Main targets - After creation, strip the ELF file and convert to DOL
#---------------------------------------------------------------------------------
$(OUTPUT).dol: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)
	$(LD) -o $@ $(OFILES) $(LIBPATHS) $(LDFLAGS) $(LIBS)
	$(STRIP) $@

#---------------------------------------------------------------------------------
-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
