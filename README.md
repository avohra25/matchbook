# matchbook

A price-time priority limit order book and matching engine in modern C++ (C++20, CMake, no dependencies).

Work in progress: this first commit contains the order book data structure (price levels, FIFO queues, O(1) cancel by id) and its tests.

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

MIT licensed.
