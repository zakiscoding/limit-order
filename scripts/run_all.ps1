$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)
$env:PYTHONPATH = "python"

python -m unittest discover -s python/tests -v
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build cpp/build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = ".\cpp\build\lob_engine.exe"
& $exe selftest --scenarios testdata\scenarios.txt --expected testdata\expected.txt
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force -Path data | Out-Null
& $exe generate --seed 42 --count 1000 --output data\cpp_1000.bin
python -m lob generate --seed 42 --count 1000 --output data\py_1000.bin
python -c "from pathlib import Path; a=Path('data/cpp_1000.bin').read_bytes(); b=Path('data/py_1000.bin').read_bytes(); assert a==b, 'generator bytes differ'; print('generator bytes match', len(a))"

& $exe generate --seed 42 --count 100000 --output data\events_100k.bin
python -m lob replay --input data\events_100k.bin --output data\py_100k.fp --checkpoint 1000 --verify
& $exe replay --input data\events_100k.bin --output data\cpp_100k.fp --checkpoint 1000 --verify
python -m lob compare data\py_100k.fp data\cpp_100k.fp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $exe bench --seed 42 --count 2000000 --runs 5
exit $LASTEXITCODE
