# Product Requirements: Batch UI

This frontend uses the `BatchResult` domain contract as source of truth.

## Batch list page (required fields)
- `batchId`
- `time`
- `status`
- `solveTimeMs`
- `residualNorm`

## Batch details page (required fields)
- `batchId`
- `time`
- `status`
- `solveTimeMs`
- `residualNorm`
- `clearPrices` (instrument -> clear price)
- `executedRates` (orderId -> executed rate)
- `fills` (array)

## Fill item fields
- `orderId` (required)
- `userId` (required)
- `instrument` (required)
- `side` (required)
- `executedQty` (required)
- `price` (required)
- `executedNotional` (required)
- `fee` (optional)

