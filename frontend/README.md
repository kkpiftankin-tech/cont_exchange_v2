# Frontend

## Services
- `frontend-web` (Node static UI + API proxy) on `http://localhost:8091`
- `frontend-api` (Node mock API) on `http://localhost:8090`

## Run with Docker
```bash
cd frontend
make up
```

## Run with Batch Simulation
Starts frontend with dynamic `/api/batches` data (new batches appear over time):

```bash
cd frontend
make up-sim
```

## Build only
```bash
cd frontend
make build
```

## Tests
Run all frontend tests:

```bash
cd frontend
make test
```

Run tests in watch mode:

```bash
cd frontend
make test-watch
```

Run a single test file:

```bash
cd frontend
make test-file TEST=src/pages/Profile/__tests__/BatchesSection.test.js
```

## Batch Simulator (60s)
Run a local API simulator that updates `/api/batches` over time and stops after 60 seconds:

```bash
cd frontend
make simulate-batches
```

To connect frontend-web to this simulator, set:

```bash
REACT_APP_API_BASE_URL=http://localhost:8092/api
```

If you use Docker from this repo, prefer `make up-sim` instead (it wires simulation into `frontend-api` directly).

## Stop
```bash
cd frontend
make down
```

## Pages
- Home: `http://localhost:8091/` (served from `/main/`)
- About: `http://localhost:8091/about/`
- External Venues: `http://localhost:8091/venues/`
- HedgeFlow Monitor: `http://localhost:8091/hedgeflows/`
- Hedge PnL: `http://localhost:8091/hedge-pnl/`
- Execution Live Feed: `http://localhost:8091/execution-live/`
- Backtest / Replay: `http://localhost:8091/replay/`
- Batch list: `http://localhost:8091/batches/`
- Batch details: `http://localhost:8091/batch/?batchId=batch-2026-03-24-0001`
