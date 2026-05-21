SHELL := /bin/bash

build:
	docker compose -f docker-compose.yml build --pull=false

up:
	docker compose -f docker-compose.yml up -d --build --pull never

up-sim:
	BATCHES_SIMULATE=1 docker compose -f docker-compose.yml up -d --build --pull never

down:
	docker compose -f docker-compose.yml down

logs:
	docker compose -f docker-compose.yml logs -f --tail=200

ps:
	docker compose -f docker-compose.yml ps

clean:
	docker compose -f docker-compose.yml down --volumes --remove-orphans

test:
	cd web && npm test -- --watchAll=false

test-watch:
	cd web && npm test

test-file:
	@if [ -z "$(TEST)" ]; then \
		echo "Usage: make test-file TEST=src/pages/Profile/__tests__/BatchesSection.test.js"; \
		exit 1; \
	fi
	cd web && npm test -- --watchAll=false $(TEST)

simulate-batches:
	node api/batches_simulator.js

.PHONY: build up up-sim down logs ps clean test test-watch test-file simulate-batches
