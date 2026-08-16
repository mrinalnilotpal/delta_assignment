#pragma once

// Common standard-library "vocabulary" headers, included once here so individual
// translation units don't each re-list <vector>/<string>/<algorithm>/... . Pulled
// in by the root headers (Types.h, OrderId.h, Logging.h, RingBuffer.h) and thus
// reaches every OMS file transitively. Single-use headers (<fstream>, <random>,
// <chrono>, <iostream>, <cstring>, ...) intentionally stay local to their file.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
