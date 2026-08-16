#pragma once

#include <iostream>

#include "oms/Common.h"

namespace oms {

enum class LogLevel : uint8_t { Info, Warn, Error };

// Sink for structured log lines. The OMS never silently applies a bad event;
// every rejection path emits one line with full context through here.
class ILogSink {
 public:
  virtual ~ILogSink() = default;
  virtual void log(LogLevel level, const std::string& line) = 0;
};

// Writes to stderr. Suitable for the sim driver / production.
class StderrLogSink : public ILogSink {
 public:
  void log(LogLevel level, const std::string& line) override {
    const char* tag = level == LogLevel::Error ? "ERROR"
                    : level == LogLevel::Warn  ? "WARN "
                                               : "INFO ";
    std::cerr << '[' << tag << "] " << line << '\n';
  }
};

// Captures lines in memory so tests can assert on rejections/anomalies.
class CapturingLogSink : public ILogSink {
 public:
  void log(LogLevel level, const std::string& line) override {
    records_.push_back({level, line});
  }

  struct Record { LogLevel level; std::string line; };
  const std::vector<Record>& records() const { return records_; }

  std::size_t count(LogLevel level) const {
    std::size_t n = 0;
    for (const auto& r : records_) n += (r.level == level);
    return n;
  }

  bool contains(const std::string& needle) const {
    for (const auto& r : records_) {
      if (r.line.find(needle) != std::string::npos) return true;
    }
    return false;
  }

  void clear() { records_.clear(); }

 private:
  std::vector<Record> records_;
};

}  // namespace oms
