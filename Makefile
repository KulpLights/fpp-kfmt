MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
MKFILE_DIR := $(dir $(MKFILE_PATH))
SRCDIR ?= /opt/fpp/src
include $(SRCDIR)/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: $(MKFILE_DIR)libfpp-kfmt.$(SHLIB_EXT)	
debug: all

OBJECTS_fpp_kfmt_so += $(MKFILE_DIR)src/FPPKFMTPlugin.o  $(MKFILE_DIR)src/QN8027.o $(MKFILE_DIR)src/CP2112.o
LIBS_fpp_kfmt_so += -L$(SRCDIR) -lfpp -ljsoncpp -lhidapi-hidraw
CFLAGS += -I$(SRCDIR) -I$(MKFILE_DIR)src

$(MKFILE_DIR)src/%.o: $(MKFILE_DIR)src/%.cpp $(MKFILE_DIR)Makefile 
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

$(MKFILE_DIR)libfpp-kfmt.$(SHLIB_EXT): $(OBJECTS_fpp_kfmt_so) $(SRCDIR)/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_kfmt_so) $(LIBS_fpp_kfmt_so) $(LDFLAGS) -o $@

clean:
	rm -f $(MKFILE_DIR)libfpp-kfmt.$(SHLIB_EXT) $(OBJECTS_fpp_kfmt_so)
