TARGET = THE_CITY_NO_SIGNAL
OBJS = src/main.o
INCDIR =
CFLAGS = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspaudio -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpspkernel -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = THE CITY: NO SIGNAL
BUILD_PRX = 1
PSP_FW_VERSION = 500

include $(PSPSDK)/lib/build.mak
