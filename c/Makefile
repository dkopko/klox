# This Makefile leverages CMake to produce the several different build types.
# This gives the ability to call 'make' in the root of the project and get
# sensible results, which allows nice integration into vim's ':make' command,
# as well as aliases like make='bear make' for integration with YouCompleteMe.

KLOX_ROOT := $(patsubst %/,%,$(dir $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))))
KLOX_BUILD_ROOT := $(KLOX_ROOT)/c/BUILD
CB_ROOT ?= $(dir $(KLOX_ROOT))/cb
CB_BUILD_ROOT := $(CB_ROOT)/BUILD

.DEFAULT_GOAL := all
.PHONY : all
all : debug release relwithdebinfo minsizerel coverage

.PHONY : echo
echo :
	@echo "KLOX_ROOT=$(KLOX_ROOT)"
	@echo "KLOX_BUILD_ROOT=$(KLOX_BUILD_ROOT)"
	@echo "CB_ROOT=$(CB_ROOT)"
	@echo "CB_BUILD_ROOT=$(CB_BUILD_ROOT)"

.PHONY : clean
clean :
	$(RM) -r "$(KLOX_BUILD_ROOT)"

.PHONY : debug
debug :
	mkdir -p "$(KLOX_BUILD_ROOT)/Debug"
	cd "$(KLOX_BUILD_ROOT)/Debug" ; cmake "$(KLOX_ROOT)/c" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$(CB_BUILD_ROOT)/Debug -DCMAKE_INCLUDE_PATH=$(CB_ROOT)/src\;$(CB_ROOT)
	$(MAKE) -C "$(KLOX_BUILD_ROOT)/Debug"

.PHONY : release
release :
	mkdir -p "$(KLOX_BUILD_ROOT)/Release"
	cd "$(KLOX_BUILD_ROOT)/Release" ; cmake "$(KLOX_ROOT)/c" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(CB_BUILD_ROOT)/Release -DCMAKE_INCLUDE_PATH=$(CB_ROOT)/src\;$(CB_ROOT)
	$(MAKE) -C "$(KLOX_BUILD_ROOT)/Release"

.PHONY : RelWithDebInfo
relwithdebinfo :
	mkdir -p "$(KLOX_BUILD_ROOT)/RelWithDebInfo"
	cd "$(KLOX_BUILD_ROOT)/RelWithDebInfo" ; cmake "$(KLOX_ROOT)/c" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=$(CB_BUILD_ROOT)/RelWithDebInfo -DCMAKE_INCLUDE_PATH=$(CB_ROOT)/src\;$(CB_ROOT)
	$(MAKE) -C "$(KLOX_BUILD_ROOT)/RelWithDebInfo"

.PHONY : minsizerel
minsizerel :
	mkdir -p "$(KLOX_BUILD_ROOT)/MinSizeRel"
	cd "$(KLOX_BUILD_ROOT)/MinSizeRel" ; cmake "$(KLOX_ROOT)/c" -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_PREFIX_PATH=$(CB_BUILD_ROOT)/MinSizeRel -DCMAKE_INCLUDE_PATH=$(CB_ROOT)/src\;$(CB_ROOT)
	$(MAKE) -C "$(KLOX_BUILD_ROOT)/MinSizeRel"

.PHONY : coverage
coverage :
	mkdir -p "$(KLOX_BUILD_ROOT)/Coverage"
	cd "$(KLOX_BUILD_ROOT)/Coverage" ; cmake "$(KLOX_ROOT)/c" -DCMAKE_BUILD_TYPE=Coverage -DCMAKE_PREFIX_PATH=$(CB_BUILD_ROOT)/Coverage -DCMAKE_INCLUDE_PATH=$(CB_ROOT)/src\;$(CB_ROOT)
	$(MAKE) -C "$(KLOX_BUILD_ROOT)/Coverage"
