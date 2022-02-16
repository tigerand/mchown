#
# Makefile for mchown program
#
# Copyright 2020-2022 Andrew Sharp andy@tigerand.com, All Rights Reserved

CC := gcc
CFLAGS+=-O2 -pthread -ftabstop=4 -fstrict-aliasing -fstrict-overflow -Wundef -Wunused-macros -Wchar-subscripts -Wcomment -Wuninitialized -Winit-self -Wunused-parameter -Wunused-but-set-parameter  -Wno-endif-labels -Wpointer-arith -Wtype-limits -Wbad-function-cast -Wcast-align -Wwrite-strings -Wsign-compare -Wsign-conversion -Wmemset-transposed-args -Waddress -Wlogical-op -Winline

ifeq ($(MAKECMDGOALS),debug)
 CFLAGS+=-g -D MDEBUG
endif

.PHONY: clean

MAIN=mchown

OBJS := mchown.o thread-pool.o
SRCS := $(OBJS:.o=.c)


$(MAIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(MAIN)

debug: $(MAIN)

DEPDIR := .d
$(shell mkdir -p $(DEPDIR) >/dev/null)
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) -c
POSTCOMPILE = @mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d && touch $@

%.o : %.c
%.o : %.c $(DEPDIR)/%.d
	$(COMPILE.c) $(OUTPUT_OPTION) $<
	$(POSTCOMPILE)

# throw away builtin rules for .cpp files, thankyouverymuch
%.o : %.cpp

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d

ifneq ($(MAKECMDGOALS),clean)
 include $(wildcard $(patsubst %,$(DEPDIR)/%.d,$(basename $(SRCS))))
endif
ifneq ($(MAKECMDGOALS),tags)
 include $(wildcard $(patsubst %,$(DEPDIR)/%.d,$(basename $(SRCS))))
endif


tags: $(SRCS)
	ctags mchown.[ch] thread-pool.c

clean:
	rm -f $(OBJS) $(MAIN)
