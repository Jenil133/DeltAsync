#ifndef DELTASYNC_TESTS_EDIT_MODEL_H_
#define DELTASYNC_TESTS_EDIT_MODEL_H_

// Generators for basis files and for the kinds of edits that turn a basis
// into a target, shared by the round-trip and stress tests.

#include <cstddef>
#include <cstdint>
#include <string>

#include "test_util.h"

namespace deltasync::test {

enum class EditKind { kInsert, kDelete, kOverwrite, kMove };
enum class Where { kStart, kMiddle, kEnd, kAnywhere };

const char* name_of(EditKind kind);
const char* name_of(Where where);

// Applies one edit of about `length` bytes (clamped to what the data
// allows). Inserted and overwritten bytes are random, so they never match
// the basis by accident. Returns the number of bytes the edit touched.
std::size_t apply_edit(Bytes& data, EditKind kind, Where where, std::size_t length, Rng& rng);

// Random bytes, or text-like data built from a small vocabulary (low
// entropy, many repeated substrings and weak-checksum near-collisions).
Bytes make_basis(std::size_t size, bool text_like, Rng& rng);

}  // namespace deltasync::test

#endif  // DELTASYNC_TESTS_EDIT_MODEL_H_
