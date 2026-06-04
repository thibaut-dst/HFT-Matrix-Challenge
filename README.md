cmake --build build -j8


Use Release mode only when needed

for development, you can use a normal build
for performance testing, use Release
example

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8