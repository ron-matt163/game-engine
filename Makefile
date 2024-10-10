GAME ?= all
ARGS ?=
SANITIZER ?=

all: build

build:
	cmake -S . -B build -DSANITIZER=$(SANITIZER)
	@ln -sf build/compile_commands.json compile_commands.json
	@echo
	cmake --build build --target $(GAME) -- -j8

clean:
	rm -rf build compile_commands.json .cache

play:
	@if [ "$(GAME)" = "all" ]; then \
		echo "Usage: make play GAME=<game>"; \
		echo "Where: <game> is one of:"; \
		cat .targetgames; \
		echo; \
		exit 1; \
	fi
	cd ./build/$(GAME) && ./$(GAME) $(ARGS)

format:
	find src -name '*.[ch]pp' | xargs -P 8 -n 1 clang-format -i

check-format:
	find src -name '*.[ch]pp' | xargs -P 8 -n 1 clang-format --dry-run --Werror

check-tidy:
	find src/engine $(shell cat .targetgames | sed 's/^/src\/games\//') -name '*.[ch]pp' -print | xargs -P 8 -n 1 clang-tidy

.PHONY: all build clean play format check-format check-tidy
