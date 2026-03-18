SHELL := /bin/bash

.PHONY: build clean

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j

clean:
	rm -rf build
