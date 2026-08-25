SHELL := /bin/bash

# ============================================================================
# Targets:
#   make build              — release C++ build
#   make test-ci            — full ctest run (isolated Docker)
#   make clean              — drop build/
#   make traceability-check — verify specs/domain links to docs and code
#   make proto-check        — verify proto-map covers all .proto files
#   make hooks-check        — smoke-test Claude Code UserPromptSubmit hook
#   make feature-check      — feature.yaml ↔ feature-component-map.yaml sync (T-AUDIT-006, planned)
#   make kafka-check        — Kafka topic surface consistency (T-AUDIT-007, planned)
#   make governance-check   — run all governance gates above
#   make help               — show this list
# Added by AUDIT-001 T-AUDIT-009.
# ============================================================================

.PHONY: help check-deps build test-ci clean \
        traceability-check proto-check hooks-check feature-check kafka-check \
        governance-check

help:
	@awk 'BEGIN{FS=":.*##"} /^[a-zA-Z_-]+:.*##/ {printf "  %-22s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@echo ""
	@echo "(Targets without ## docstrings: build, test-ci, clean, check-deps.)"

check-deps: ## verify host build dependencies
	./scripts/check_deps.sh

build: check-deps ## release C++ build into build/
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j

test-ci: ## full ctest run in isolated Docker
	./scripts/test_ci.sh

clean: ## drop build/ directory
	rm -rf build

# ----- governance gates (read-only checkers) --------------------------------

traceability-check: ## verify specs/domain links to docs and code
	python3 tools/traceability-checker/check.py

proto-check: ## verify proto-map covers all .proto files
	python3 tools/proto-contract-auditor/check_proto_map.py

hooks-check: ## smoke-test Claude Code UserPromptSubmit hook
	python3 tools/auto-archive-attachments.py --self-test

feature-check: ## (planned T-AUDIT-006) feature.yaml ↔ feature-component-map.yaml sync
	@if [ -x tools/feature-yaml-checker/check.py ]; then \
	    python3 tools/feature-yaml-checker/check.py; \
	else \
	    echo "feature-yaml-checker not implemented yet (AUDIT-001 T-AUDIT-006)"; \
	fi

kafka-check: ## (planned T-AUDIT-007) Kafka topic surface consistency
	@if [ -x tools/kafka-contract-auditor/check.py ]; then \
	    python3 tools/kafka-contract-auditor/check.py; \
	else \
	    echo "kafka-contract-auditor not implemented yet (AUDIT-001 T-AUDIT-007)"; \
	fi

governance-check: traceability-check proto-check hooks-check feature-check kafka-check ## run all governance gates
	@echo ""
	@echo "governance-check OK"
