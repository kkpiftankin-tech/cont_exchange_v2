SHELL := /bin/bash

.PHONY: check-deps build test-ci clean

check-deps:
	./scripts/check_deps.sh

build: check-deps
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j

test-ci:
	./scripts/test_ci.sh

clean:
	rm -rf build
