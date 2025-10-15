SRCDIR ?= /opt/fpp/src
include $(SRCDIR)/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-kfmt.$(SHLIB_EXT)
debug: all

OBJECTS_fpp_kfmt_so += src/FPPKFMTPlugin.o  src/QN8027.o
LIBS_fpp_kfmt_so += -L$(SRCDIR) -lfpp -ljsoncpp
CFLAGS += -I$(SRCDIR)

%.o: %.cpp Makefile 
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

%.o: %.c Makefile 
	$(CCACHE) gcc $(CFLAGS) $(CFLAGS_$@) -c $< -o $@

libfpp-kfmt.$(SHLIB_EXT): $(OBJECTS_fpp_kfmt_so) $(SRCDIR)/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_kfmt_so) $(LIBS_fpp_kfmt_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-kfmt.$(SHLIB_EXT) $(OBJECTS_fpp_kfmt_so)
