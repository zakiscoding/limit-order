# Limit order book and market-data replay

Single-symbol limit order book with deterministic replay. The C++ engine and the Python reference apply the same rules to the same event log and emit the same fingerprints.

## Matching rules

Prices are integer ticks. Quantities are positive integers. The book never reads the wall clock. Time priority is the event timestamp, stored as position in a FIFO at each price.

- Bids match highest price first. Asks match lowest price first. The same price matches oldest order first.
- A trade prints at the resting order's price. A GTC order rests whatever quantity does not trade. An IOC order does not rest.
- Cancel removes a resting order. A missing id is rejected and the book does not change.
- Modify keeps priority only when the price is unchanged and the remaining quantity gets smaller. Any price change, or any increase in quantity, pulls the order out and places it at the tail of its new level. That re-entry can take liquidity. Quantity zero is rejected; cancel is a separate event.
- `RESET` is a replay marker. It clears resting orders and leaves the cumulative trade fingerprint in place.

Rejects cover a duplicate id, an unknown id, a non-positive price or quantity, a bad side, a bad time-in-force, and an unknown event type.

## Replay file

`LOBREPV1`, little-endian. A 32-byte header (`magic`, version 1, header size 32, event count, seed) is followed by 32-byte events:

| offset | field |
| --- | --- |
| 0 | type: 1 add, 2 cancel, 3 modify, 4 reset |
| 1 | side: 0 buy, 1 sell |
| 2 | time in force: 0 GTC, 1 IOC |
| 4 | quantity, uint32 |
| 8 | order id, uint64 |
| 16 | price ticks, int64 |
| 24 | logical timestamp, uint64 |

Text scenarios live in `testdata/scenarios.txt`. The hand-checked trades and book states are in `testdata/expected.txt`. Both engines read that file.

The generator is SplitMix64. For a non-empty live id set the next event is cancel (35%), modify (10%), or add (55%). Adds walk a fair price by one tick inside 8000..12000; 30% are priced through that fair value. The generator does not know about fills, so many cancels and modifies name orders the book has already removed. Both engines must reject those the same way. The C++ generator is a direct port of `python/lob/generate.py`.

## Build and check

Requirements: Python 3.12, CMake, Ninja, Clang.

```powershell
$env:PYTHONPATH = "python"
python -m unittest discover -s python/tests -v

cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build cpp/build
.\cpp\build\lob_engine.exe selftest --scenarios testdata\scenarios.txt --expected testdata\expected.txt
```

`scripts/run_all.ps1` runs the unit tests, the byte-level generator check, a 100,000-event fingerprint compare, and the benchmark.

```powershell
.\cpp\build\lob_engine.exe generate --seed 42 --count 100000 --output data\events.bin
python -m lob replay --input data\events.bin --output data\py.fp --checkpoint 1000 --verify
.\cpp\build\lob_engine.exe replay --input data\events.bin --output data\cpp.fp --checkpoint 1000 --verify
python -m lob compare data\py.fp data\cpp.fp

.\cpp\build\lob_engine.exe bench --seed 42 --count 2000000 --runs 5
```

A fingerprint line is the event index, the book hash, the trade hash, trade count, best bid and ask with size, resting order count, and reject count. The book hash walks bids from best to worst, then asks from best to worst, and each order front to back. The mix is a 64-bit xor-then-multiply, the same function in both languages.

Phase timers are a separate build. They are not the throughput numbers.

```powershell
cmake -S cpp -B cpp/build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DLOB_INSTRUMENT=ON
cmake --build cpp/build-prof
.\cpp\build-prof\lob_engine.exe profile --seed 42 --count 2000000 --runs 1
```

## Layout

- `cpp/` — matcher, generator, replay, benchmark
- `python/lob/` — reference matcher and the same generator
- `testdata/` — scripted events and the expected trades and book
- `results/RESULTS.md` — hardware, workload, profile, and timings

## Performance

On this machine the back-to-back measurement, seed 42 and 2,000,000 events, moved from 12.40 million events/s with `std::unordered_map` to 17.18 million after the id index became an open-addressed table. The book hash and trade hash did not change (`510a7332d2e46339` and `f5122ddf7ab89668`). Mean time per event went from 80.6 ns to 58.2 ns. The full setup, the profile that picked the bottleneck, and the timer limits are in [results/RESULTS.md](results/RESULTS.md).
