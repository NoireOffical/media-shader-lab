CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

LOCAL_ENV := $(CURDIR)/.tools/env
VMAF_PREFIX := $(CURDIR)/.tools/vmaf
FFMPEG_VMAF_PREFIX := $(CURDIR)/.tools/ffmpeg-vmaf

ifneq ($(wildcard $(LOCAL_ENV)/bin/cmake),)
CMAKE_BIN := $(LOCAL_ENV)/bin/cmake
CTEST_BIN := $(LOCAL_ENV)/bin/ctest
ifneq ($(wildcard $(FFMPEG_VMAF_PREFIX)/bin/ffmpeg),)
LOCAL_PREFIX_PATH := $(FFMPEG_VMAF_PREFIX);$(VMAF_PREFIX);$(LOCAL_ENV)
LOCAL_PKG_CONFIG_PATH := $(FFMPEG_VMAF_PREFIX)/lib/pkgconfig:$(VMAF_PREFIX)/lib/pkgconfig:$(LOCAL_ENV)/lib/pkgconfig
LOCAL_PATH := $(FFMPEG_VMAF_PREFIX)/bin:$(LOCAL_ENV)/bin:$(PATH)
else
LOCAL_PREFIX_PATH := $(LOCAL_ENV)
LOCAL_PKG_CONFIG_PATH := $(LOCAL_ENV)/lib/pkgconfig
LOCAL_PATH := $(LOCAL_ENV)/bin:$(PATH)
endif
CMAKE_CONFIG_ARGS := -G Ninja \
	-DCMAKE_PREFIX_PATH="$(LOCAL_PREFIX_PATH)" \
	-DPKG_CONFIG_EXECUTABLE=$(LOCAL_ENV)/bin/pkg-config \
	-DCMAKE_MAKE_PROGRAM=$(LOCAL_ENV)/bin/ninja
else
CMAKE_BIN := cmake
CTEST_BIN := ctest
CMAKE_CONFIG_ARGS :=
LOCAL_PKG_CONFIG_PATH := $(PKG_CONFIG_PATH)
LOCAL_PATH := $(PATH)
endif

.PHONY: test-core check-deps configure build test benchmark clean

test-core:
	@mkdir -p build/core
	$(CXX) $(CXXFLAGS) -Iinclude src/core/Metrics.cpp tests/MetricsTest.cpp -o build/core/metrics_test
	./build/core/metrics_test

check-deps:
	bash scripts/check_dependencies.sh

configure:
	PATH="$(LOCAL_PATH)" PKG_CONFIG_PATH="$(LOCAL_PKG_CONFIG_PATH)" \
		$(CMAKE_BIN) -S . -B build -DCMAKE_BUILD_TYPE=Release $(CMAKE_CONFIG_ARGS)

build: configure
	$(CMAKE_BIN) --build build --parallel

test: build
	$(CTEST_BIN) --test-dir build --output-on-failure

benchmark: build
	python3 scripts/benchmark_pipeline.py

clean:
	$(RM) build/core/metrics_test
