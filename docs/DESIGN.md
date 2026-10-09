# Deltasync design

Deltasync brings an old version of a file (the *basis*) up to date with a new
version (the *target*) by sending only what changed. It uses the rsync
algorithm: the side that holds the basis describes it with a compact
*signature*, the side that holds the target finds every basis block that
reappears in the target and produces a *delta* of block references and new
bytes, and the basis side rebuilds the target from the two and verifies it.

This document covers the algorithm, the on-disk formats, and how output is
verified and committed. The command-line tool exposes the three steps
separately, like `rdiff`:

```
deltasync signature BASIS SIGNATURE      # on the side that has the old version
deltasync delta SIGNATURE TARGET DELTA   # on the side that has the new version
deltasync patch BASIS DELTA OUTPUT       # back on the old side
```

## Checksums

### Weak rolling checksum

The weak checksum is the rsync checksum (Tridgell & Mackerras, 1996). For a
window `x[0..n)` with `y[i] = x[i] + 31`:

```
a = sum(y[i])              mod 2^16
b = sum((n - i) * y[i])    mod 2^16
weak = a | (b << 16)
```

Sliding the window one byte, from `x[k..k+n)` to `x[k+1..k+n+1)`, is O(1):

```
a' = a - y[k] + y[k+n]
b' = b - n * y[k] + a'
```

The offset of 31 (librsync's value) keeps zero bytes from contributing
nothing, so runs of zeros of different lengths get different checksums.
The sums live in 32-bit accumulators that wrap mod 2^32; because 2^16
divides 2^32, reducing to 16 bits only when the digest is read gives the same
result as reducing after every step (`include/deltasync/rolling_checksum.h`).

The weak checksum is only a filter. It is cheap enough to evaluate at every
byte offset of the target, but it is easy to collide: adding `(+1, -2, +1)`
to three consecutive bytes leaves both sums unchanged. Every weak match is
therefore confirmed with the strong hash before a block is reused.

### Strong hash

SHA-256 (FIPS 180-4) is implemented in-tree (`src/sha256.cpp`). It is
validated against the FIPS 180-2 SHA-256 examples (one block, the 448-bit
message whose padding spills into a second block, one million `a`), the
896-bit message from the FIPS 180-2 SHA-512 examples (its SHA-256 digest
cross-checked with `shasum -a 256` and `openssl dgst -sha256`), NIST CAVP
short messages, and split-update properties. The
byte counter is 64 bits wide; a unit test resumes the hasher from midstates
positioned around 2^29, 2^32, 2^40 and 2^61 bytes and checks the padding
against hand-built blocks, and the `stress` suite hashes a real 1 GiB message
and a 4 GiB + 3 byte message whose expected digests were confirmed with
`openssl dgst -sha256` (OpenSSL 3.6.3) and `shasum -a 256`.

SHA-256 is used three ways:

| Use | Length | Purpose |
| --- | --- | --- |
| Per-block strong checksum in the signature | 16 bytes by default (8..32) | confirms a weak match before a block is reused |
| Per-chunk digest in the delta (`CHUNK`) | 32 bytes | lets the receiver verify output chunk by chunk |
| Whole-target digest in the delta (`END`) | 32 bytes | final end-to-end check; equals `sha256sum` of the target |

Truncating the per-block hash to 128 bits keeps signatures at 20 bytes per
block while leaving accidental collisions far out of reach. The whole-file
hash is never truncated.

## Block size

Unless `--block-size` is given, the block size for a basis of `S` bytes is

```
B = clamp(ceil_to_1KiB(floor(sqrt(S))), 1 KiB, 128 KiB)
```

| Basis size | Default block size |
| --- | --- |
| up to 1 MiB | 1 KiB |
| 64 MiB | 8 KiB |
| 100 MiB | 10 KiB |
| 1 GiB | 32 KiB |
| 16 GiB and larger | 128 KiB |

Each changed region costs roughly one block of literal data, while the
signature costs 20 bytes per block, so for `E` edits the transfer
`20 * S / B + E * B` is smallest near `B = sqrt(20 * S / E)`. `sqrt(S)` is
rsync's heuristic for a moderate number of edits. The lower clamp keeps the
signature of a small file from being dominated by per-block overhead; the
upper clamp bounds the cost of a single edit in a very large file and keeps
the encoder's buffer small. The policy lives in `default_block_size()`
(`include/deltasync/signature.h`); `deltasync signature --block-size SIZE`
overrides it (1 byte to 16 MiB, with K/M/G suffixes).

## Signature

The signature records the basis length, the block size, and for every block
`(weak checksum, truncated SHA-256)`. Every block is `B` bytes except
possibly the last, which is shorter.

Signatures are streams: `SignatureGenerator` consumes the basis in pieces of
any size and emits records to a `SignatureSink` while holding one block of
memory. Sinks include `SignatureWriter` (serializes), `SignatureBuilder`
(collects an in-memory `Signature` for the encoder), and `SignatureDecoder`
parses serialized input split at arbitrary byte boundaries back into the
same callbacks.

### Signature format

All integers are unsigned LEB128 varints (minimal encodings only) unless
noted otherwise.

```
magic          "DSIG" (4 bytes)
version        1 (1 byte)
flags          0 (varint; nonzero is rejected)
block_size     varint, 1 .. 16 MiB
strong_length  varint, 8 .. 32
basis_size     varint
block_count x  { weak: 4 bytes big-endian, strong: strong_length bytes }
```

`block_count = ceil(basis_size / block_size)` is implied, so truncation and
trailing bytes are both detected without a trailer. A header that implies
more than 2^31 blocks is rejected before anything is allocated for it.

## Delta

### Matching

The encoder (`src/delta_encoder.cpp`) indexes the signature by weak checksum
in an open-addressing hash table at most half full, so the lookup that
happens at almost every byte offset usually ends at the first or second
slot. Blocks sharing a weak checksum form a chain in block order. A block
whose length and strong checksum equal one already in its chain is left out
(runs of identical blocks, such as zero-filled regions, cost one candidate
instead of thousands), and a chain stops growing at 64 distinct blocks, which
only matters for adversarial weak collisions and can only enlarge the delta,
never corrupt it.

A window of `B` bytes then slides over the target:

1. Look up the window's weak checksum. A miss (the common case) moves the
   window one byte; the byte it leaves joins the pending literal.
2. On a hit, compute SHA-256 of the window once and compare its prefix with
   the candidates of the same length. The block after the previous match is
   tried first, so runs of repeated blocks stay contiguous in the basis and
   coalesce into one COPY.
3. On a confirmed match, emit the pending literal, emit `COPY`, jump the
   window a whole block ahead, and recompute its checksum from scratch.

A final basis block shorter than `B` can only match where the target ends,
so after the last full window there is exactly one place to try it.

The strong hash therefore runs only at weak hits. In one run of the release
build on macOS (Apple M5) over two independent 64 MiB files read from
`/dev/urandom` (`deltasync signature basis sig`, then
`deltasync delta --stats sig unrelated delta`; 8 KiB blocks), the weak
checksum was evaluated at about 67 million offsets and SHA-256 was needed at
350 of them, all rejected: `weak_hits=350 false_matches=350`. The exact count
depends on the random data.

### Records and chunks

The delta is a stream of records:

| Record | Meaning |
| --- | --- |
| `COPY(offset, length)` | output basis bytes `[offset, offset + length)` |
| `LITERAL(bytes)` | output bytes carried in the delta |
| `CHUNK(sha256)` | SHA-256 of the output since the previous chunk boundary |
| `END(length, sha256)` | total target length and SHA-256 of the whole target |

COPY offsets are byte offsets, not block numbers, so consecutive matching
blocks merge into a single record and a record can be split at any byte.

The output is divided into chunks of `chunk_size` bytes (default 1 MiB)
counted from target offset 0. No COPY or LITERAL crosses a chunk boundary,
and a CHUNK record follows the last operation of every chunk, including a
final short one. A receiver can therefore verify and persist the output one
chunk at a time, and a transfer can start at any chunk boundary: the header's
`start_offset` says where in the target the stream begins, and both the
encoder and the patcher accept the SHA-256 state of the bytes before it
(`Sha256::Midstate` makes that state storable), so END still certifies the
whole file.

Literals are flushed whenever 64 KiB are pending (configurable), which also
bounds the encoder's memory. Records never need more than a few bytes of
lookahead and literal bytes are passed through as they arrive, so both
encoding and decoding work in bounded memory regardless of file size.

### Delta format

```
header   magic "DSDL" | version 1 (1 byte) | flags 0 (varint) |
         chunk_size (varint, a multiple of 64 up to 1 GiB) | basis_size (varint) |
         start_offset (varint, a multiple of chunk_size)
COPY     0x01 | offset (varint) | length (varint)
LITERAL  0x02 | length (varint) | length bytes
CHUNK    0x03 | 32-byte SHA-256
END      0x04 | total_length (varint) | 32-byte SHA-256
```

Besides syntax, `DeltaDecoder` enforces the structure a patcher relies on:
operations are non-empty, COPY ranges lie inside the declared basis, nothing
crosses a chunk boundary, nothing follows a short (final) chunk except END,
every output byte is covered by a CHUNK before END, END's length matches the
operations, and nothing follows END. Any violation, truncation or unknown
record type raises `FormatError`.

## Patching

`Patcher` is a `DeltaSink`: it checks that the header's `basis_size` matches
the basis it was given, executes COPY with positional reads of the basis and
LITERAL by passing bytes through, and hashes every output byte into both a
chunk hash and a whole-file hash. A CHUNK digest that does not match raises
`IntegrityError` naming the byte range, so a wrong basis or a corrupted delta
is caught at the first chunk it affects; END is checked against the
whole-file hash. An optional callback reports each verified prefix length;
during the call `Patcher::file_hash()` holds the SHA-256 state of exactly
that prefix, which is what a resume journal stores. Chunk sizes are
multiples of 64 bytes (the SHA-256 block length) so that this state can be
exported as a `Sha256::Midstate` at every chunk boundary.

Because output is written before its chunk is verified, `patch_file()`
writes into an `AtomicFile`: a temporary file created with `O_EXCL` and mode
0600 next to the destination, so a private file is never exposed through a
more permissive temporary copy. Only after END verifies does it flush, apply
the final permissions (those of the file being replaced, otherwise those of
the basis; without either, what a new file in that directory would get),
sync the data (`F_FULLFSYNC` on macOS, `fdatasync` on Linux, `fsync`
elsewhere), rename over the destination and sync the directory (again
`F_FULLFSYNC` first on macOS). On any error
the temporary file is removed and the destination is untouched. The
destination may be the basis itself: the open descriptor keeps reading the
old file while the new one is renamed into place.

## Resume: durable partial output

`PartialOutput` (`include/deltasync/resume.h`) is the sink a receiving
`Patcher` writes into when a transfer must survive interruption. It keeps
two hidden files next to the destination `<dir>/<name>`:

- `<dir>/.<name>.partial`: the target bytes written so far (mode 0600 until
  commit);
- `<dir>/.<name>.journal`: 128 bytes recording how much of the partial file
  is verified.

The unit of resumption is the chunk (default 1 MiB, `push --chunk-size`):
"the last verified block" a transfer resumes from is the last chunk whose
`CHUNK` checksum passed and was made durable, not a signature block.

Pushed names never start with `.`, so this state cannot collide with a file
someone pushed, and visible files such as `<name>.partial` are never
touched. A data file is only truncated, replaced or removed while a journal
claims it: fresh state is created with `O_EXCL` and claimed at once by a
journal that verifies 0 bytes, and a data file with no journal next to it
(not made by deltasync, or left by a crash in the instant between creating
and claiming it) is left alone; opening fails with `UnclaimedPartialFile`
and the receiver answers `ERROR` (I/O).

```
journal  magic "DSJR" | version 1 (1 byte) | 3 zero bytes |
         target size (u64) | target SHA-256 (32) | chunk size (u64) |
         verified length (u64) | SHA-256 midstate words (8 x u32) |
         SHA-256 of the preceding 96 bytes (32)          (big-endian)
```

The target is identified by its size, whole-file SHA-256 and chunk size
(`TargetId`), all of which the sender announces before any data. After each
verified chunk the patcher's callback runs `checkpoint()`: flush and sync
the partial file, then write a complete new journal to `.<name>.journal.tmp`,
sync it, rename it over the journal and sync the directory. Data is always
durable before the journal that vouches for it, and the rename makes the
journal switch atomic, so after a crash at any point the journal on disk
claims at most what is on stable storage. Only full chunks are journaled;
the final short chunk is followed at once by END and `commit()`.

Opening a `PartialOutput` resumes only if all of these hold, and otherwise
replaces the journal with a fresh claim (one rename), empties the partial
file and starts from offset 0:

1. the journal is exactly 128 bytes, with the right magic, version and
   checksum;
2. it describes the same target (size, digest and chunk size);
3. its verified length is a chunk multiple no larger than the target;
4. the partial file holds at least that many bytes, and those bytes hash to
   the recorded midstate.

The last check rereads the verified prefix once per resume (reporting its
progress to an optional callback, which the receiver turns into `PROGRESS`
frames). Without it,
damage to the partial file under an intact journal would pass END, because
the patcher continues the whole-file hash from the journal's state instead
of rehashing the bytes on disk. Bytes beyond the verified length were never
confirmed and are cut off. `verified_length()` and `prefix_state()` are what
the receiver reports to the sender, which checks the state against its own
file's prefix and encodes from that offset with `DeltaEncoder(prefix)`; the
receiver continues with `Patcher(basis, partial, partial.prefix_hash())`.

`commit()` flushes, applies the final permissions, syncs, renames the partial
file over the destination, syncs the directory and removes the journal (a
journal that survives a crash at that point finds no partial file and is
discarded). `abandon()` removes the data file, then the journal (so a crash
in between never leaves unclaimed data); destroying the object without
either keeps them for the next attempt, and `rewind()` returns a live object
to its last checkpoint for a retry in the same process. An exclusive
`flock()` on the partial file keeps two transfers, in any processes, from
writing the same destination at once.

Tests (`tests/resume_test.cpp`) interrupt transfers after every possible
number of chunks and at random delta bytes, reopen, and check the resume
offset, the prefix state, that the sender's remaining delta is smaller than
a full one, and that the result is the exact target. Every single-byte
corruption of the journal, journals for another target or of another
version, and truncated, missing or damaged partial data all restart from
zero and still finish correctly. An unclaimed data file is left as it was,
and visible look-alike files survive a whole transfer. Crash ordering is checked by killing a
forked process with `_exit()` between the data sync and the journal
replacement and right after it: the reopened state is the previous and the
new checkpoint respectively, never more.

## Memory and allocation

| Component | Memory |
| --- | --- |
| Signature generator | one block |
| Signature in memory | 20 bytes per block (default strong length) |
| Block index | 8 bytes per slot, at least 2 slots per block, plus 4 bytes per block |
| Delta encoder | fixed buffer of `2 * (max_literal + block_size)` bytes (144 KiB for 8 KiB blocks) |
| Delta decoder | constant; literal bytes are not buffered |
| Patcher | 256 KiB copy buffer |

After construction, signature generation, delta encoding, delta decoding and
patching perform no heap allocation; a dedicated test binary replaces the
global `operator new` with a counting version and checks this over an 8 MiB
file with scattered edits.

## Errors and exit codes

Library errors derive from `deltasync::Error`: `FormatError` for malformed
or unsupported input, `IntegrityError` when data fails verification, and
`IoError` (with `errno` and the path) when a system call fails. The command
line tool prints `deltasync: <message>` and exits with 0 on success, 1 when
the operation failed, and 2 for invalid usage. With `--stats` each command
prints one line of `key=value` pairs to standard error, for example (release
build, a 64 MiB `/dev/urandom` file with one 4 KiB overwrite aligned to a
block, so a single block is resent):

```
delta: target_bytes=67108864 matched_bytes=67100672 literal_bytes=8192 delta_bytes=10866 copy_ops=64 literal_ops=1 chunks=64 block_size=8192 weak_hits=8191 false_matches=0 seconds=...
```

## Example: a small edit to a large file

`ctest --preset release -R LargeFileTest -V` runs the encoder and patcher on
a 64 MiB random file (8 KiB blocks, so 8,192 blocks and a 163,853-byte
signature) with one 4 KiB edit. The results are deterministic because the
data comes from a seeded generator:

| Edit | Literal bytes | Delta bytes | Delta / file size |
| --- | --- | --- | --- |
| overwrite straddling two blocks | 16,384 | 19,059 | 0.028% |
| insertion | 12,288 | 15,002 | 0.022% |
| deletion | 12,288 | 14,962 | 0.022% |

Most of the delta is the literal data of the damaged blocks; the 64 CHUNK
records add 33 bytes per MiB of output.

## Network sync

`deltasync serve --port P --root DIR` is the receiver: it holds the basis
files under `DIR`. `deltasync push FILE --host H --port P [--name NAME]` is
the sender: it holds the new version of one file. The code lives in
`src/net.cpp` (sockets), `src/protocol.cpp` (frames and messages),
`src/sync_client.cpp` and `src/sync_server.cpp`.

### Wire protocol

Every message is a frame: `type (1 byte) | payload length (4 bytes,
big-endian) | payload`, with payloads of at most 1 MiB; a larger announced
length or an unknown type is rejected before anything is buffered.
Fixed-width integers inside payloads are big-endian.

```
sender                                  receiver
HELLO "DSYN" version=1 role=sender  ->
FILE_INFO                           ->
                                    <-  HELLO "DSYN" version=1 role=receiver
                                    <-  SIGNATURE x n   (n = 0: no signature)
                                    <-  RESUME offset, prefix midstate
DATA x m (the serialized delta)     ->
                                    <-  OK | ERROR

PROGRESS frames (receiver -> sender) may appear anywhere after the
receiver's HELLO; the sender skips them.
```

- `FILE_INFO`: mode (`0` delta, `1` whole-file) | chunk size (8) | requested
  block size (4, `0` = receiver's default) | file size (8) | SHA-256 of the
  file (32) | name length (2) | name.
- `SIGNATURE` frames carry the serialized signature of the basis (the format
  above), streamed as the receiver reads its file; a frame ends when it is
  full (256 KiB) or after every 8 MiB of basis. There are none when the
  basis is missing or empty or in whole-file mode; the sender then encodes
  against an empty basis, so every byte travels as a LITERAL through the same
  path. `RESUME` ends the signature.
- `RESUME`: verified offset (8) | SHA-256 midstate words of the verified
  prefix (8 x 4). The sender accepts an offset only if it is a chunk multiple
  within the file and the midstate equals the hash of its own first `offset`
  bytes; otherwise it starts at 0. The delta header's `start_offset` tells
  the receiver which one it chose.
- `DATA` frames carry the delta (`DeltaWriter` output). A frame ends when it
  is full (256 KiB) and after every `CHUNK` record (5 bytes of framing per
  chunk), so even a chunk that encodes to a few dozen bytes reaches the
  receiver as soon as it is encoded. The receiver feeds their payload to `DeltaDecoder` -> `Patcher` as it
  arrives, not frame by frame, so a connection that breaks loses at most the
  chunk in progress. It checks that the header's chunk size matches, that its
  start offset is the offered resume point or 0, that no more than the
  announced size is produced, and that `END` certifies the announced SHA-256.
- `PROGRESS`: position (8), how far the receiver's current step has got
  (prefix bytes checked, or target bytes verified); informational only.
- `OK` carries the receiver's own byte counts; `ERROR` carries a code
  (protocol, version, bad request, busy, integrity, I/O, internal) and a
  message of at most 1 KiB. Either side may send `ERROR` instead of its next
  frame. The sender also checks for an incoming `ERROR` before each `DATA`
  frame, and the receiver reads what is still in flight after sending one,
  so the sender sees the reason instead of a connection reset.

The sender sends `HELLO` and `FILE_INFO` without waiting, so a sync costs
two round trips plus the transfer.

### Staying live under the inactivity timeout

Each side gives up on a peer that sends nothing for `--timeout-ms` (default
30 s). Work proportional to the file size must therefore never happen in
silence:

| Phase | Who waits | What keeps it alive |
| --- | --- | --- |
| sender hashes its file for `FILE_INFO` | (nobody) | done before connecting |
| receiver hashes the basis for the signature | sender | a `SIGNATURE` frame after every 8 MiB of basis |
| receiver rehashes resumable partial data before `RESUME` | sender | `PROGRESS` at most once a second |
| sender checks a `RESUME` offer | receiver | rereads less than 1/4096 of the file (see below) |
| sender encodes | receiver | a `DATA` frame after every `CHUNK` record |
| receiver rebuilds, verifies and checkpoints after the sender's `END` | sender | `PROGRESS` at most once a second, then `OK` |

The receiver sends `PROGRESS` only after a second without any other frame
(`ServerOptions::progress_interval_ms`), so it costs at most 13 bytes per
second and can never fill the sender's receive buffer while the sender is
busy writing. The sender skips `PROGRESS` before `RESUME`, between its
`DATA` frames (it drains anything pending before each frame) and before
`OK`. A sender timeout shorter than a few seconds can still expire while a
large chunk is being verified and synced; such timeouts are not supported.

### Receiver

Names are one plain path component of at most 242 bytes: no `/`, no control
characters, and not starting with `.` (which rules out `.`, `..` and the
receiver's hidden temporary and resume files; 242 + 13 bytes of
`.<name>.journal.tmp` is the usual 255-byte name limit). An existing
destination that is not a regular file (a symlink, a directory) is refused
rather than followed.

The new version is written through `PartialOutput` (see "Resume: durable
partial output") into `.<name>.partial` next to the destination, with a
checkpoint after every verified chunk, and is only renamed into place after
the `END` hash verified and the data is synced; the destination stays
untouched until then. The replaced file's permission bits are kept. What
happens to the partial state when an attempt fails depends on why:

- the connection broke (disconnect, reset, timeout, server stop, including a
  failure to send `RESUME`): the verified chunks and their journal are kept
  for the next attempt, unless nothing was verified, in which case both
  files are removed;
- anything else (a chunk or `END` that fails verification, a malformed or
  mis-announced delta, an `ERROR` from the sender): both files are removed,
  so a bad attempt is never resumed.

`run()` handles each connection on its own thread (16 at once by default;
more are refused with `ERROR busy`, sent from the accepting thread, which
then drains the refused connection for at most 2 s in total). Two connections writing the same name
at once would race to the final rename, so the second one is refused with
`ERROR busy` while the first is active; the `flock()` on the partial file
does the same across processes. `serve --once` handles one connection on
the main thread and exits with its status. The server prints
`listening on ADDR:PORT` on standard output (useful with `--port 0`) and one
line per connection on standard error (with `resumed_from` on success).
There is no authentication or encryption: run it on trusted networks only.

### Resuming an interrupted push

1. After the signature, the receiver opens `PartialOutput` for the target
   that `FILE_INFO` announced (size, SHA-256, chunk size). If an earlier
   attempt left a journal for exactly this target and the partial data still
   hashes to it, it offers `RESUME(verified length, midstate)`; otherwise
   `RESUME(0)`.
2. The sender checks the offer against its own first `offset` bytes and
   accepts it only if the midstate matches (and the offset is a chunk
   multiple within the file). Its first pass over the file, made before it
   connected, kept the SHA-256 state at up to 4,097 evenly spaced chunk
   boundaries (40 bytes each), so the check continues from the nearest
   saved state and rereads less than one stride (1/4096 of the file, or one
   chunk if that is more) instead of the whole prefix. It then encodes from there with `DeltaEncoder(prefix)`, so the
   delta's `start_offset` is the offset and `END` still certifies the whole
   file. Otherwise it encodes from 0.
3. The receiver sees the choice in the delta header. A header at the offered
   offset continues with `Patcher(basis, partial, partial.prefix_hash())`; a
   header at 0 means the sender declined, so the receiver abandons the
   partial state, reopens it fresh and rebuilds from scratch. Any other
   start offset is a protocol error.
4. `push --retries N` reconnects after a broken connection (or a busy
   receiver) and the new attempt goes through the same steps, so it resumes
   automatically. If the connection died without the receiver noticing (no
   FIN or RST reached it), the receiver keeps that session, and the name,
   until its inactivity timeout expires and answers `ERROR busy` meanwhile.
   With `--retries` of at least 1, busy answers are therefore retried
   without using up the N retries until `--timeout-ms` has passed since the
   first of them (backing off up to 5 s between attempts). `--stats-json` reports `attempts` and `resumed_from` (the
   offset of the successful attempt); byte counts add up all attempts.

The basis is never modified before commit, so the resumed attempt's `COPY`
records still refer to the same basis the signature describes. The
signature is sent again on every attempt; only the target bytes already
verified are skipped.

Tests (`tests/resume_sync_test.cpp`) run a real `SyncServer` and
`push_file()` through an in-process forwarder that cuts the connection after
an exact number of sender-to-receiver bytes. They interrupt a 2 MiB
whole-file push at 13 points, from inside the handshake to the last byte,
and check after each resumed push that the result is the exact file, that
no partial state is left, that the resume offset is a chunk multiple no
larger than what was delivered and at most one chunk (plus framing) below
it, that only the remaining bytes were sent as literals, and that the
resumed push plus the resume offset costs no more than a full transfer
(for cuts past half the file, the resumed push sends less than half). Other
tests cover a delta-mode push resumed against an unchanged basis,
`--retries` reconnecting through a cut connection with `attempts == 2`, a
changed file restarting from zero, a receiver offering a prefix the sender
does not have, and damaged partial data. `scripts/netem_bench.sh` runs the
same scenario with the real binaries through `deltasync_proxy`.

### Sockets

Every blocking call waits in `poll()` first, with a per-operation inactivity
timeout (`--timeout-ms`, default 30 s, 0 = none), retries after `EINTR`, and
loops over partial reads and writes. Writing to a closed peer is reported as
an error, not `SIGPIPE`: `send()` uses `MSG_NOSIGNAL` on Linux and sockets
get `SO_NOSIGPIPE` on macOS. Memory is bounded on both ends: the signature
and the delta are streamed frame by frame, the receiver holds one control
frame (at most 1 MiB) or a 64 KiB piece of a `DATA` frame plus the
patcher's buffer, and the sender holds the
signature (refusing one with more than 2^23 blocks) plus the encoder's fixed
buffer. The sender reads its file twice per attempt: once before it
connects, for the whole-file hash that `FILE_INFO` announces and the saved
hash states, and once to encode.

### Byte accounting

Both ends count every byte their socket reads and writes. `push --stats` and
`--stats-json PATH` report `bytes_sent` / `bytes_received` (bytes written
to / read from the socket: the TCP payload, including frame headers and the
handshake, but not TCP/IP headers or retransmissions), `signature_bytes` and
`delta_bytes` (frame payloads), `literal_bytes` / `matched_bytes` (from the
encoder), `resumed_from`, `attempts` and `elapsed_ms`. With `--retries N`
the sender reconnects after network failures or a busy receiver; byte counts
then add up all attempts. A loopback test
(`SyncServerTest.LargeFileWithASmallEditSendsKilobytes`, debug build, macOS,
three runs) pushed a 64 MiB random file with a 4 KiB overwrite: 11,265 bytes
went from sender to receiver every time (delta 10,866, of which 8,192
literal, in 65 `DATA` frames), so the sender's traffic was 0.017% of the
file. 164,009 to 164,048 bytes came back: the signature (163,853 bytes in 8
frames), `HELLO`, `RESUME` and `OK` make 163,970, plus 3 to 6 `PROGRESS`
frames of 13 bytes, depending on how long the receiver took.

### Measurements

The sender's byte counts are deterministic and asserted by tests: the
64 MiB / 4 KiB edit loopback push above, and in
`tests/resume_sync_test.cpp` the bound that a resumed push plus its resume
offset costs no more than one full transfer. The receiver's counts are too,
except for its `PROGRESS` frames (13 bytes each), whose number depends on
how long it works. Wall-time and socket byte counts under emulated latency
and loss come from
`scripts/netem_bench.sh` (whole-file baseline, delta after a small edit,
interrupted and resumed push over a delay x loss grid with `tc netem`);
the method, exact commands, environment and result table are in
`docs/BENCHMARKS.md`.

### Known issues

- A target window whose weak checksum matches a basis block but whose strong
  hash does not is a false alarm. Inside a run of one repeated byte every
  window is the same, so the encoder remembers the miss and hashes the run
  once (a basis block crafted to collide with a constant run used to force
  a SHA-256 of the whole window at every offset). Other repeating patterns
  change the window at every step; for them the encoder stops hashing a
  weak value after 1,024 consecutive false alarms on it, until something
  matches. Such data can therefore lose a genuine match of that one block
  later on (a larger delta, never a wrong one).
- Chunk sizes must be multiples of 64 bytes, because resuming restores the
  file hash from a SHA-256 midstate.
- Resume granularity is the chunk size (default 1 MiB, `push --chunk-size`):
  an interrupted attempt keeps only whole verified chunks, so it can lose up
  to one chunk of transferred data, and a file no larger than one chunk
  always restarts from zero. Smaller chunks resume more precisely at the
  cost of a 33-byte `CHUNK` record and a journal checkpoint (data and
  journal fsync) per chunk.
- Every attempt resends the receiver's signature; only the verified target
  prefix is skipped.
- A hidden `.<name>.partial` without a journal is never overwritten, so one
  left behind by a crash in the instant between creating it and writing its
  claim (or whose unsynced claim was lost in a power failure) blocks pushes
  of that name with an I/O error until it is removed by hand.
- `PROGRESS` is sent at most once a second and only between verified chunks,
  so a sender whose `--timeout-ms` is only a few seconds can still time out
  while the receiver verifies and syncs a large chunk.
