app = champsim

srcExt = cc
srcDir = src branch replacement prefetcher
objDir = obj
binDir = bin
inc = inc

debug = 1

CFlags = -Wall -O3 -std=c++11
# Capture the source revision when make starts.  The escaped quotes make these
# values safe C string literals, including the detached-HEAD case.
GIT_COMMIT := $(shell git rev-parse --verify HEAD 2>/dev/null || echo unknown)
GIT_BRANCH := $(shell git symbolic-ref --short -q HEAD 2>/dev/null || echo detached)
CFlags += -DCHAMPSIM_GIT_COMMIT=\"$(GIT_COMMIT)\" -DCHAMPSIM_GIT_BRANCH=\"$(GIT_BRANCH)\"
LDFlags =
libs =
libDir =


#************************ DO NOT EDIT BELOW THIS LINE! ************************

ifeq ($(debug),1)
	debug=-g
else
	debug=
endif
inc := $(addprefix -I,$(inc))
libs := $(addprefix -l,$(libs))
libDir := $(addprefix -L,$(libDir))
CFlags += -c $(debug) $(inc) $(libDir) $(libs)
sources := $(shell find $(srcDir) -name '*.$(srcExt)')
srcDirs := $(shell find . -name '*.$(srcExt)' -exec dirname {} \; | uniq)
objects := $(patsubst %.$(srcExt),$(objDir)/%.o,$(sources))

ifeq ($(srcExt),cc)
	CC = $(CXX)
else
	CFlags += -std=gnu99
endif

.phony: all clean distclean


all: $(binDir)/$(app)

$(binDir)/$(app): buildrepo $(objects)
	@mkdir -p `dirname $@`
	@echo "Linking $@..."
	@$(CC) $(objects) $(LDFlags) -o $@

$(objDir)/%.o: %.$(srcExt)
	@echo "Generating dependencies for $<..."
	@$(call make-depend,$<,$@,$(subst .o,.d,$@))
	@echo "Compiling $<..."
	@$(CC) $(CFlags) $< -o $@

clean:
	$(RM) -r $(objDir)

distclean: clean
	$(RM) -r $(binDir)/$(app)

buildrepo:
	@$(call make-repo)

define make-repo
   for dir in $(srcDirs); \
   do \
	mkdir -p $(objDir)/$$dir; \
   done
endef


# usage: $(call make-depend,source-file,object-file,depend-file)
define make-depend
  $(CC) -MM       \
        -MF $3    \
        -MP       \
        -MT $2    \
        $(CFlags) \
        $1
endef
