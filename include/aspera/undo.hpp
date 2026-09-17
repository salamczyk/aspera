#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace aspera {
// Byte-bounded snapshot history shared by all editor authoring tools.
class UndoHistory {
  public:
    explicit UndoHistory(size_t budget = 256u * 1024u * 1024u, size_t entries = 96)
        : budget_(budget), entries_(entries) {}
    void record(std::vector<uint8_t> bytes) {
        for (auto &entry : redo_)
            bytes_ -= entry.bytes.size();
        redo_.clear();
        push(undo_, std::move(bytes));
        trim();
    }
    std::optional<std::vector<uint8_t>> undo(std::vector<uint8_t> current) {
        return transfer(undo_, redo_, std::move(current));
    }
    std::optional<std::vector<uint8_t>> redo(std::vector<uint8_t> current) {
        return transfer(redo_, undo_, std::move(current));
    }
    bool canUndo() const {
        return !undo_.empty();
    }
    bool canRedo() const {
        return !redo_.empty();
    }
    size_t bytes() const {
        return bytes_;
    }
    size_t count() const {
        return undo_.size() + redo_.size();
    }

  private:
    struct Entry {
        uint64_t serial;
        std::vector<uint8_t> bytes;
    };
    std::deque<Entry> undo_, redo_;
    size_t budget_, entries_, bytes_{};
    uint64_t serial_{};
    void push(std::deque<Entry> &history, std::vector<uint8_t> bytes) {
        bytes_ += bytes.size();
        history.push_back({serial_++, std::move(bytes)});
    }
    void drop(std::deque<Entry> &history) {
        bytes_ -= history.front().bytes.size();
        history.pop_front();
    }
    void trim() {
        while (undo_.size() > entries_)
            drop(undo_);
        while (redo_.size() > entries_)
            drop(redo_);
        while (bytes_ > budget_ && count()) {
            if (redo_.empty() || (!undo_.empty() && undo_.front().serial < redo_.front().serial))
                drop(undo_);
            else
                drop(redo_);
        }
    }
    std::optional<std::vector<uint8_t>> transfer(std::deque<Entry> &from, std::deque<Entry> &to,
                                                 std::vector<uint8_t> current) {
        if (from.empty())
            return std::nullopt;
        auto result = std::move(from.back().bytes);
        bytes_ -= result.size();
        from.pop_back();
        push(to, std::move(current));
        trim();
        return result;
    }
};
} // namespace aspera
