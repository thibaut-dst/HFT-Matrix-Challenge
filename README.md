cmake --build build -j8


Use Release mode only when needed

for development, you can use a normal build
for performance testing, use Release
example

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8




------------


parse_ms = from first byte of the challenge to the parser finishing that challenge

compute_ms = from parse completion until all workers have finished

send_ms = time spent sending the response

total_latency_ms = first byte to response fully sent
