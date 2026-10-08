#include "edit_model.h"

#include <algorithm>

namespace deltasync::test {
namespace {

// A position for an edit spanning `length` bytes of data of `size` bytes:
// in [0, size - length].
std::size_t pick_position(std::size_t size, std::size_t length, Where where, Rng& rng) {
  const std::size_t last = size - length;
  switch (where) {
    case Where::kStart:
      return 0;
    case Where::kEnd:
      return last;
    case Where::kMiddle: {
      // Near the middle, jittered so edits do not always land on the same
      // block alignment.
      const std::size_t jitter = std::min<std::size_t>(31, last / 2);
      return last / 2 - jitter + rng.below(2 * jitter + 1);
    }
    case Where::kAnywhere:
      return rng.between(0, last);
  }
  return 0;
}

}  // namespace

const char* name_of(EditKind kind) {
  switch (kind) {
    case EditKind::kInsert:
      return "insert";
    case EditKind::kDelete:
      return "delete";
    case EditKind::kOverwrite:
      return "overwrite";
    case EditKind::kMove:
      return "move";
  }
  return "?";
}

const char* name_of(Where where) {
  switch (where) {
    case Where::kStart:
      return "start";
    case Where::kMiddle:
      return "middle";
    case Where::kEnd:
      return "end";
    case Where::kAnywhere:
      return "anywhere";
  }
  return "?";
}

std::size_t apply_edit(Bytes& data, EditKind kind, Where where, std::size_t length, Rng& rng) {
  const auto at = [&](std::size_t offset) {
    return data.begin() + static_cast<std::ptrdiff_t>(offset);
  };
  switch (kind) {
    case EditKind::kInsert: {
      Bytes inserted(length);
      rng.fill(inserted.data(), inserted.size());
      const std::size_t position = pick_position(data.size(), 0, where, rng);
      data.insert(at(position), inserted.begin(), inserted.end());
      return length;
    }
    case EditKind::kDelete: {
      length = std::min(length, data.size());
      const std::size_t position = pick_position(data.size(), length, where, rng);
      data.erase(at(position), at(position + length));
      return length;
    }
    case EditKind::kOverwrite: {
      length = std::min(length, data.size());
      const std::size_t position = pick_position(data.size(), length, where, rng);
      rng.fill(data.data() + position, length);
      return length;
    }
    case EditKind::kMove: {
      length = std::min(length, data.size());
      const std::size_t source = pick_position(data.size(), length, where, rng);
      const Bytes moved(at(source), at(source + length));
      data.erase(at(source), at(source + length));
      const std::size_t destination = rng.between(0, data.size());
      data.insert(at(destination), moved.begin(), moved.end());
      return length;
    }
  }
  return 0;
}

Bytes make_basis(std::size_t size, bool text_like, Rng& rng) {
  if (!text_like) {
    Bytes data(size);
    rng.fill(data.data(), data.size());
    return data;
  }
  static const char* const kWords[] = {
      "the",    "block",  "rolling", "checksum", "delta",   "copy",    "literal", "chunk",
      "sync",   "basis",  "target",  "file",     "server",  "client",  "network", "byte",
      "offset", "length", "hash",    "strong",   "weak",    "match",   "window",  "buffer",
      "stream", "record", "verify",  "resume",   "journal", "latency", "loss",    "packet",
      "a",      "of",     "to",      "and",      "is",      "in",      "it",      "for"};
  constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
  Bytes data;
  data.reserve(size + 16);
  std::size_t words_on_line = 0;
  while (data.size() < size) {
    const char* word = kWords[rng.below(kWordCount)];
    data.insert(data.end(), word, word + std::char_traits<char>::length(word));
    if (++words_on_line >= 5 + rng.below(8)) {
      data.push_back('\n');
      words_on_line = 0;
    } else {
      data.push_back(' ');
    }
  }
  data.resize(size);
  return data;
}

}  // namespace deltasync::test
