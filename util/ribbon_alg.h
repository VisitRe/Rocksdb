//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <array>

#include "util/math128.h"

namespace ROCKSDB_NAMESPACE {

namespace ribbon {

// RIBBON PHSF & RIBBON Filter (Rapid Incremental Boolean Banding ON-the-fly)
//
// ribbon_alg.h: generic versions of core algorithms.
//
// Ribbon is a Perfect Hash Static Function construction useful as a compact
// static Bloom filter alternative. It combines (a) a boolean (GF(2)) linear
// system construction that approximates a Band Matrix with hashing,
// (b) an incremental, on-the-fly Gaussian Elimination algorithm that is
// remarkably efficient and adaptable at constructing an upper-triangular
// band matrix from a set of band-approximating inputs from (a), and
// (c) a storage layout that is fast and adaptable as a filter.
//
// Footnotes: (a) "Efficient Gauss Elimination for Near-Quadratic Matrices
// with One Short Random Block per Row, with Applications" by Stefan
// Walzer and Martin Dietzfelbinger ("DW paper")
// (b) developed by Peter C. Dillinger, though not the first on-the-fly
// GE algorithm. See "On the fly Gaussian Elimination for LT codes" by
// Bioglio, Grangetto, Gaeta, and Sereno.
// (c) TODO: not yet implemented here
//
// See ribbon_impl.h for high-level behavioral summary. This file focuses
// on the core design details.
//
// ######################################################################
// ################# PHSF -> static filter reduction ####################
//
// A Perfect Hash Static Function is a data structure representing a
// map from anything hashable (a "key") to values of some fixed size.
// Crucially, it is allowed to return garbage values for anything not in
// the original set of map keys, and it is a "static" structure: entries
// cannot be added or deleted after construction. PHSFs representing n
// mappings to b-bit values (assume uniformly distributed) require at least
// n * b bits to represent, or at least b bits per entry. We typically
// describe the compactness of a PHSF by typical bits per entry as some
// function of b. For example, the MWHC construction (k=3 "peeling")
// requires about 1.0222*b and a variant called Xor+ requires about
// 1.08*b + 0.5 bits per entry.
//
// With more hashing, a PHSF can over-approximate a set as a Bloom filter
// does, with no FN queries and predictable false positive (FP) query
// rate. Instead of the user providing a value to map each input key to,
// a hash function provides the value. Keys in the original set will
// return a positive membership query because the underlying PHSF returns
// the same value as hashing the key. When a key is not in the original set,
// the PHSF returns a "garbage" value, which is only equal to the key's
// hash with (false positive) probability 1 in 2^b.
//
// For a matching false positive rate, standard Bloom filters require
// 1.44*b bits per entry. Cache-local Bloom filters (like bloom_impl.h)
// require a bit more, around 1.5*b bits per entry. Thus, a Bloom
// alternative could save up to or nearly 1/3rd of memory and storage
// that RocksDB uses for SST (static) Bloom filters. (Memtable Bloom filter
// is dynamic.)
//
// Recommended reading:
// "Xor Filters: Faster and Smaller Than Bloom and Cuckoo Filters"
// by Graf and Lemire
// First three sections of "Fast Scalable Construction of (Minimal
// Perfect Hash) Functions" by Genuzio, Ottaviano, and Vigna
//
// ######################################################################
// ################## PHSF vs. hash table vs. Bloom #####################
//
// You can think of traditional hash tables and related filter variants
// such as Cuckoo filters as utilizing an "OR" construction: a hash
// function associates a key with some slots and the data is returned if
// the data is found in any one of those slots. The collision resolution
// is visible in the final data structure and requires extra information.
// For example, Cuckoo filter uses roughly 1.05b + 2 bits per entry, and
// Golomb-Rice code (aka "GCS") as little as b + 1.5. When the data
// structure associates each input key with data in one slot, the
// structure implicitly constructs a (near-)minimal (near-)perfect hash
// (MPH) of the keys, which requires at least 1.44 bits per key to
// represent. This is why approaches with visible collision resolution
// have a fixed + 1.5 or more in storage overhead per entry, often in
// addition to an overhead multiplier on b.
//
// By contrast Bloom filters utilize an "AND" construction: a query only
// returns true if all bit positions associated with a key are set to 1.
// There is no collision resolution, so Bloom filters do not suffer a
// fixed bits per entry overhead like the above structures.
//
// PHSFs typically use a bitwise XOR construction: the data you want is
// not in a single slot, but in a linear combination of several slots.
// For static data, this gives the best of "AND" and "OR" constructions:
// avoids the +1.44 or more fixed overhead by not approximating a MPH and
// can do much better than Bloom's 1.44 factor on b with collision
// resolution, which here is done ahead of time and invisible at query
// time.
//
// ######################################################################
// ######################## PHSF construction ###########################
//
// For a typical PHSF, construction is solving a linear system of
// equations, typically in GF(2), which is to say that values are boolean
// and XOR serves both as addition and subtraction. We can use matrices to
// represent the problem:
//
//    C    *    S    =    R
// (n x m)   (m x b)   (n x b)
// where C = coefficients, S = solution, R = results
// and solving for S given C and R.
//
// Note that C and R each have n rows, one for each input entry for the
// PHSF. A row in C is given by a hash function on the PHSF input key,
// and the corresponding row in R is the b-bit value to associate with
// that input key. (In a filter, rows of R are given by another hash
// function on the input key.)
//
// On solving, the matrix S (solution) is the final PHSF data, as it
// maps any row from the original C to its corresponding desired result
// in R. We just have to hash our query inputs and compute a linear
// combination of rows in S.
//
// In theory, we could chose m = n and let a hash function associate
// each input key with random rows in C. A solution exists with high
// probability, and uses essentially minimum space, b bits per entry
// (because we set m = n) but this has terrible scaling, something
// like O(n^2) space and O(n^3) time during construction (Gaussian
// elimination) and O(n) query time. But computational efficiency is
// key, and the core of this is avoiding scanning all of S to answer
// each query.
//
// The traditional approach (MWHC, aka Xor filter) starts with setting
// only some small fixed number of columns (typically k=3) to 1 for each
// row of C, with remaining entries implicitly 0. This is implemented as
// three hash functions over [0,m), and S can be implemented as a vector
// vector of b-bit values. Now, a query only involves looking up k rows
// (values) in S and computing their bitwise XOR. Additionally, this
// construction can use a linear time algorithm called "peeling" for
// finding a solution in many cases of one existing, but peeling
// generally requires a larger space overhead factor in the solution
// (m/n) than is required with Gaussian elimination.
//
// Recommended reading:
// "Peeling Close to the Orientability Threshold – Spatial Coupling in
// Hashing-Based Data Structures" by Stefan Walzer
//
// ######################################################################
// ##################### Ribbon PHSF construction #######################
//
// Ribbon constructs coefficient rows essentially the same as in the
// Walzer/Dietzfelbinger paper cited above: for some chosen fixed width
// r (kCoeffBits in code), each key is hashed to a starting column in
// [0, m - r] (GetStart() in code) and an r-bit sequence of boolean
// coefficients (GetCoeffRow() in code). If you sort the rows by start,
// the C matrix would look something like this:
//
// [####00000000000000000000]
// [####00000000000000000000]
// [000####00000000000000000]
// [0000####0000000000000000]
// [0000000####0000000000000]
// [000000000####00000000000]
// [000000000####00000000000]
// [0000000000000####0000000]
// [0000000000000000####0000]
// [00000000000000000####000]
// [00000000000000000000####]
//
// where each # could be a 0 or 1, chosen uniformly by a hash function.
// (Except we typically set the start column value to 1.) This scheme
// uses hashing to approximate a band matrix, and it has a solution iff
// it reduces to an upper-triangular boolean r-band matrix, like this:
//
// [1###00000000000000000000]
// [01##00000000000000000000]
// [000000000000000000000000]
// [0001###00000000000000000]
// [000000000000000000000000]
// [000001##0000000000000000]
// [000000000000000000000000]
// [00000001###0000000000000]
// [000000001###000000000000]
// [0000000001##000000000000]
// ...
// [00000000000000000000001#]
// [000000000000000000000001]
//
// where we have expanded to an m x m matrix by filling with rows of
// all zeros as needed. As in Gaussian elimination, this form is ready for
// generating a solution through back-substitution.
//
// The awesome thing about the Ribbon construction (from the DW paper) is
// how row reductions keep each row representable as a start column and
// r coefficients, because row reductions are only needed when two rows
// have the same number of leading zero columns. Thus, the combination
// of those rows, the bitwise XOR of the r-bit coefficient rows, cancels
// out the leading 1s, so starts (at least) one column later and only
// needs (at most) r - 1 coefficients.
//
// ######################################################################
// ###################### Ribbon PHSF scalability #######################
//
// Although more practical detail is in ribbon_impl.h, it's worth
// understanding some of the overall benefits and limitations of the
// Ribbon PHSFs.
//
// High-end scalability is a primary issue for Ribbon PHSFs, because in
// a single Ribbon linear system with fixed r and fixed m/n ratio, the
// solution probability approaches zero as n approaches infinity.
// For a given n, solution probability improves with larger r and larger
// m/n.
//
// By contrast, peeling-based PHSFs have somewhat worse storage ratio
// or solution probability for small n (less than ~1000). This is
// especially true with spatial-coupling, where benefits are only
// notable for n on the order of 100k or 1m or more.
//
// To make best use of current hardware, r=128 seems to be closest to
// a "generally good" choice for Ribbon, at least in RocksDB where SST
// Bloom filters typically hold around 10-100k keys, and almost always
// less than 10m keys. r=128 ribbon has a high chance of encoding success
// (with first hash seed) when storage overhead is around 5% (m/n ~ 1.05)
// for roughly 10k - 10m keys in a single linear system. r=64 only scales
// up to about 10k keys with the same storage overhead. Construction and
// access times for r=128 are similar to r=64. r=128 tracks nearly
// twice as much data during construction, but in most cases we expect
// the scalability benefits of r=128 vs. r=64 to make it preferred.
//
// A natural approach to scaling Ribbon beyond ~10m keys is splitting
// (or "sharding") the inputs into multiple linear systems with their
// own hash seeds. This can also help to control peak memory consumption.
// TODO: much more to come
//
// ######################################################################
// #################### Ribbon on-the-fly banding #######################
//
// "Banding" is what we call the process of reducing the inputs to an
// upper-triangluar r-band matrix ready for finishing a solution with
// back-substitution. Although the DW paper presents an algorithm for
// this ("SGauss"), the awesome properties of their construction enable
// an even simpler, faster, and more backtrackable algorithm. In simplest
// terms, the SGauss algorithm requires sorting the inputs by start
// columns, but it's possible to make Gaussian elimination resemble hash
// table insertion!
//
// The enhanced algorithm is based on these observations:
// - When processing a coefficient row with first 1 in column j,
//   - If it's the first at column j to be processed, it can be part of
//     the banding at row j. (And that descision never overwritten, with
//     no loss of generality!)
//   - Else, it can be combined with existing row j and re-processed,
//     which will look for a later "empty" row or reach "no solution".
//
// We call our banding algorithm "incremental" and "on-the-fly" because
// (like hash table insertion) we are "finished" after each input
// processed, with respect to all inputs processed so far. Although the
// band matrix is an intermediate step to the solution structure, we have
// eliminated intermediate steps and unnecessary data tracking for
// banding.
//
// Building on "incremental" and "on-the-fly", the banding algorithm is
// easily backtrackable because no (non-empty) rows are overwritten in
// the banding. Thus, if we want to "try" adding an additional set of
// inputs to the banding, we only have to record which rows were written
// in order to efficiently backtrack to our state before considering
// the additional set. (TODO: how this can mitigate scalability and
// reach sub-1% overheads)
//
// Like in a linear-probed hash table, as the occupancy approaches and
// surpasses 90-95%, collision resolution dominates the construction
// time. (Ribbon doesn't usually pay at query time; see solution
// storage below.) This means that we can speed up construction time
// by using a higher m/n ratio, up to negative returns around 1.2.
// At m/n ~= 1.2, which still saves memory substantially vs. Bloom
// filter's 1.5, construction speed (including back-substitution) is not
// far from sorting speed, but still a few times slower than cache-local
// Bloom construction speed.
//
// Back-substitution from an upper-triangular boolean band matrix is
// especially fast and easy. All the memory accesses are sequential or at
// least local, no random. If the number of result bits (b) is a
// compile-time constant, the back-substitution state can even be tracked
// in CPU registers. Regardless of the solution representation, we prefer
// column-major representation for tracking back-substitution state, as
// r (the band width) will typically be much larger than b (result bits
// or columns), so better to handle r-bit values b times (per solution
// row) than b-bit values r times.
//
// ######################################################################
// ##################### Ribbon solution storage ########################
//
// Row-major layout is typical for boolean (bit) matrices, including for
// MWHC (Xor) filters where a query combines k b-bit values, and k is
// typically smaller than b. Even for k=4 and b=2, at least k=4 random
// lookups are required regardless of layout.
//
// Ribbon PHSFs are quite different, however, because
// (a) all of the solution rows relevant to a query are within a single
// range of r rows, and
// (b) the number of solution rows involved (r/2 on average, or r if
// avoiding conditional accesses) is typically much greater than
// b, the number of solution columns.
//
// Row-major for Ribbon PHSFs therefore tends to incur undue CPU overhead
// by processing (up to) r entries of b bits each, where b is typically
// less than 10 for filter applications.
//
// Column-major layout has poor locality because of accessing up to b
// memory locations in different pages (and obviously cache lines). Note
// that negative filter queries do not typically need to access all
// solution columns, as they can return when a mismatch is found in any
// result/solution column. This optimization doesn't always pay off on
// recent hardware, where the penalty for unpredictable conditional
// branching can exceed the penalty for unnecessary work, but the
// optimization is essentially unavailable with row-major layout.
//
// The best compromise seems to be interleaving column-major on the small
// scale with row-major on the large scale. For example, let a solution
// "block" be r rows column-major encoded as b r-bit values in sequence.
// Each query accesses (up to) 2 adjacent blocks, which will typically
// span 1-3 cache lines in adjacent memory. We get very close to the same
// locality as row-major, but with much faster reconstruction of each
// result column, at least for filter applications where b is relatively
// small and negative queries can return early.
//
// ######################################################################
// ###################### Fractional result bits ########################
//
// Bloom filters have great flexibility that alternatives mostly do not
// have. One of those flexibilities is in utilizing any ratio of data
// structure bits per key. With a typical memory allocator like jemalloc,
// this flexibility can save roughly 10% of the filters' footprint in
// DRAM by rounding up and down filter sizes to minimize memory internal
// fragmentation (see optimize_filters_for_memory RocksDB option).
//
// At first glance, PHSFs only offer a whole number of bits per "slot"
// (m rather than number of keys n), but coefficient locality in the
// Ribbon construction makes fractional bits/key quite possible and
// attractive for filter applications.
//
// TODO: more detail
//

// ######################################################################
// ################### CODE: Ribbon core algorithms #####################
// ######################################################################
//
// These algorithms are templatized for genericity but near-maximum
// performance in a given application. The template parameters
// adhere to class/struct type concepts outlined below.

// Rough architecture for these algorithms:
//
// +-----------+     +---+     +-----------------+
// | AddInputs | --> | H | --> | BandingStorage  |
// +-----------+     | a |     +-----------------+
//                   | s |             |
//                   | h |      Back substitution
//                   | e |             V
// +-----------+     | r |     +-----------------+
// | Query Key | --> |   | >+< | SolutionStorage |
// +-----------+     +---+  |  +-----------------+
//                          V
//                     Query result

// Common to other concepts
// concept RibbonTypes {
//   // An unsigned integer type for an r-bit subsequence of coefficients.
//   // r (or kCoeffBits) is taken to be sizeof(CoeffRow) * 8, as it would
//   // generally only hurt scalability to leave bits of CoeffRow unused.
//   typename CoeffRow;
//   // An unsigned integer type big enough to hold a result row (b bits,
//   // or number of solution/result columns).
//   // In many applications, especially filters, the number of result
//   // columns is decided at run time, so ResultRow simply needs to be
//   // big enough for the largest number of columns allowed.
//   typename ResultRow;
//   // An unsigned integer type sufficient for representing the number of
//   // rows in the solution structure. (TODO: verify any extra needed?)
//   typename Index;
// };

// ######################################################################
// ######################## Hashers and Banding #########################

// Hasher concepts abstract out hashing details.

// concept PhsfQueryHasher extends RibbonTypes {
//   // Type for a lookup key, which is hashable.
//   typename Key;
//
//   // Type for hashed summary of a Key. uint64_t is recommended.
//   typename Hash;
//
//   // Compute a hash value summarizing a Key
//   Hash GetHash(const Key &) const;
//
//   // Given a hash value and a number of columns that can start an
//   // r-sequence of coefficients (== m - r + 1), return the start
//   // column to associate with that hash value. (Starts can be chosen
//   // uniformly or "smash" extra entries into the beginning and end for
//   // better utilization at those extremes of the structure. Details in
//   // ribbon.impl.h)
//   Index GetStart(Hash, Index num_starts) const;
//
//   // Given a hash value, return the r-bit sequence of coefficients to
//   // associate with it. It's generally OK if
//   //   sizeof(CoeffRow) > sizeof(Hash)
//   // as long as the hash itself is not too prone to collsions for the
//   // applications and the CoeffRow is generated uniformly from
//   // available hash data, but relatively independent of the start.
//   //
//   // Must be non-zero, because that's required for a solution to exist
//   // when mapping to non-zero result row. (Note: BandingAdd could be
//   // modified to allow 0 coeff row if that only occurs with 0 result
//   // row, which really only makes sense for filter implementation,
//   // where both values are hash-derived. Or BandingAdd could reject 0
//   // coeff row, forcing next seed, but that has potential problems with
//   // generality/scalability.)
//   CoeffRow GetCoeffRow(Hash) const;
// };

// concept FilterQueryHasher extends PhsfQueryHasher {
//   // For building or querying a filter, this returns the expected
//   // result row associated with a hashed input. For general PHSF,
//   // this must return 0.
//   //
//   // Although not strictly required, there's a slightly better chance of
//   // solver success if result row is masked down here to only the bits
//   // actually needed.
//   ResultRow GetResultRowFromHash(Hash) const;
// }

// concept BandingHasher extends FilterQueryHasher {
//   // For a filter, this will generally be the same as Key.
//   // For a general PHSF, it must either
//   // (a) include a key and a result it maps to (e.g. in a std::pair), or
//   // (b) GetResultRowFromInput looks up the result somewhere rather than
//   // extracting it.
//   typename AddInput;
//
//   // Instead of requiring a way to extract a Key from an
//   // AddInput, we require getting the hash of the Key part
//   // of an AddInput, which is trivial if AddInput == Key.
//   Hash GetHash(const AddInput &) const;
//
//   // For building a non-filter PHSF, this extracts or looks up the result
//   // row to associate with an input. For filter PHSF, this must return 0.
//   ResultRow GetResultRowFromInput(const AddInput &) const;
//
//   // Whether the solver can assume the lowest bit of GetCoeffRow is
//   // always 1. When true, it should improve solver efficiency slightly.
//   static bool kFirstCoeffAlwaysOne;
// }

// Abstract storage for the the result of "banding" the inputs (Gaussian
// elimination to an upper-triangular boolean band matrix). Because the
// banding is an incremental / on-the-fly algorithm, this also represents
// all the intermediate state between input entries.
//
// concept BandingStorage extends RibbonTypes {
//   // Tells the banding algorithm to prefetch memory associated with
//   // the next input before processing the current input. Generally
//   // recommended iff the BandingStorage doesn't easily fit in CPU
//   // cache.
//   bool UsePrefetch() const;
//
//   // Prefetches (e.g. __builtin_prefetch) memory associated with a
//   // slot index i.
//   void Prefetch(Index i) const;
//
//   // Returns a pointer to CoeffRow for slot index i.
//   CoeffRow* CoeffRowPtr(Index i);
//
//   // Returns a pointer to ResultRow for slot index i. (Gaussian row
//   // operations involve both side of the equation.)
//   ResultRow* ResultRowPtr(Index i);
//
//   // Returns the number of columns that can start an r-sequence of
//   // coefficients, which is the number of slots minus r (kCoeffBits)
//   // plus one. (m - r + 1)
//   Index GetNumStarts() const;
// };

// Optional storage for backtracking data in banding a set of input
// entries. It exposes an array structure which will generally be
// used as a stack. It must be able to accommodate as many entries
// as are passed in as inputs to `BandingAddRange`.
//
// concept BacktrackStorage extends RibbonTypes {
//   // If false, backtracking support will be disabled in the algorithm.
//   // This should preferably be an inline compile-time constant function.
//   bool UseBacktrack() const;
//
//   // Records `to_save` as the `i`th backtrack entry
//   void BacktrackPut(Index i, Index to_save);
//
//   // Recalls the `i`th backtrack entry
//   Index BacktrackGet(Index i) const;
// }

// Adds a single entry to BandingStorage (and optionally, BacktrackStorage),
// returning true if successful or false if solution is impossible with
// current hasher (and presumably its seed) and number of "slots" (solution
// or banding rows). (A solution is impossible when there is a linear
// dependence among the inputs that doesn't "cancel out".)
//
// Pre- and post-condition: the BandingStorage represents a band matrix
// ready for back substitution (row echelon form except for zero rows),
// augmented with result values such that back substitution would give a
// solution satisfying all the cr@start -> rr entries added.
template <bool kFirstCoeffAlwaysOne, typename BandingStorage,
          typename BacktrackStorage>
bool BandingAdd(BandingStorage *bs, typename BandingStorage::Index start,
                typename BandingStorage::ResultRow rr,
                typename BandingStorage::CoeffRow cr, BacktrackStorage *bts,
                typename BandingStorage::Index *backtrack_pos) {
  using CoeffRow = typename BandingStorage::CoeffRow;
  using Index = typename BandingStorage::Index;

  Index i = start;

  if (!kFirstCoeffAlwaysOne) {
    // Requires/asserts that cr != 0
    int tz = CountTrailingZeroBits(cr);
    i += static_cast<Index>(tz);
    cr >>= tz;
  } else {
    assert((cr & 1) == 1);
  }

  for (;;) {
    CoeffRow other = *(bs->CoeffRowPtr(i));
    if (other == 0) {
      *(bs->CoeffRowPtr(i)) = cr;
      *(bs->ResultRowPtr(i)) = rr;
      bts->BacktrackPut(*backtrack_pos, i);
      ++*backtrack_pos;
      return true;
    }
    assert((other & 1) == 1);
    cr ^= other;
    rr ^= *(bs->ResultRowPtr(i));
    if (cr == 0) {
      // Inconsistency or (less likely) redundancy
      break;
    }
    int tz = CountTrailingZeroBits(cr);
    i += static_cast<Index>(tz);
    cr >>= tz;
  }
  // Failed, unless result row == 0 because e.g. a duplicate input or a
  // stock hash collision, with same result row. (For filter, stock hash
  // collision implies same result row.) Or we could have a full equation
  // equal to sum of other equations, which is very possible with
  // small range of values for result row.
  return rr == 0;
}

// Adds a range of entries to BandingStorage returning true if successful
// or false if solution is impossible with current hasher (and presumably
// its seed) and number of "slots" (solution or banding rows). (A solution
// is impossible when there is a linear dependence among the inputs that
// doesn't "cancel out".) Here "InputIterator" is an iterator over AddInputs.
//
// If UseBacktrack in the BacktrackStorage, this function call rolls back
// to prior state on failure. If !UseBacktrack, some subset of the entries
// will have been added to the BandingStorage, so best considered to be in
// an indeterminate state.
//
template <typename BandingStorage, typename BacktrackStorage,
          typename BandingHasher, typename InputIterator>
bool BandingAddRange(BandingStorage *bs, BacktrackStorage *bts,
                     const BandingHasher &bh, InputIterator begin,
                     InputIterator end) {
  using CoeffRow = typename BandingStorage::CoeffRow;
  using Index = typename BandingStorage::Index;
  using ResultRow = typename BandingStorage::ResultRow;
  using Hash = typename BandingHasher::Hash;

  static_assert(IsUnsignedUpTo128<CoeffRow>::value, "must be unsigned");
  static_assert(IsUnsignedUpTo128<Index>::value, "must be unsigned");
  static_assert(IsUnsignedUpTo128<ResultRow>::value, "must be unsigned");

  constexpr bool kFCA1 = BandingHasher::kFirstCoeffAlwaysOne;

  if (begin == end) {
    // trivial
    return true;
  }

  const Index num_starts = bs->GetNumStarts();

  InputIterator cur = begin;
  Index backtrack_pos = 0;
  if (!bs->UsePrefetch()) {
    // Simple version, no prefetch
    for (;;) {
      Hash h = bh.GetHash(*cur);
      Index start = bh.GetStart(h, num_starts);
      ResultRow rr =
          bh.GetResultRowFromInput(*cur) | bh.GetResultRowFromHash(h);
      CoeffRow cr = bh.GetCoeffRow(h);

      if (!BandingAdd<kFCA1>(bs, start, rr, cr, bts, &backtrack_pos)) {
        break;
      }
      if ((++cur) == end) {
        return true;
      }
    }
  } else {
    // Pipelined w/prefetch
    // Prime the pipeline
    Hash h = bh.GetHash(*cur);
    Index start = bh.GetStart(h, num_starts);
    ResultRow rr = bh.GetResultRowFromInput(*cur);
    bs->Prefetch(start);

    // Pipeline
    for (;;) {
      rr |= bh.GetResultRowFromHash(h);
      CoeffRow cr = bh.GetCoeffRow(h);
      if ((++cur) == end) {
        if (!BandingAdd<kFCA1>(bs, start, rr, cr, bts, &backtrack_pos)) {
          break;
        }
        return true;
      }
      Hash next_h = bh.GetHash(*cur);
      Index next_start = bh.GetStart(next_h, num_starts);
      ResultRow next_rr = bh.GetResultRowFromInput(*cur);
      bs->Prefetch(next_start);
      if (!BandingAdd<kFCA1>(bs, start, rr, cr, bts, &backtrack_pos)) {
        break;
      }
      h = next_h;
      start = next_start;
      rr = next_rr;
    }
  }
  // failed; backtrack (if implemented)
  if (bts->UseBacktrack()) {
    while (backtrack_pos > 0) {
      --backtrack_pos;
      Index i = bts->BacktrackGet(backtrack_pos);
      *(bs->CoeffRowPtr(i)) = 0;
      // Not required: *(bs->ResultRowPtr(i)) = 0;
    }
  }
  return false;
}

// Adds a range of entries to BandingStorage returning true if successful
// or false if solution is impossible with current hasher (and presumably
// its seed) and number of "slots" (solution or banding rows). (A solution
// is impossible when there is a linear dependence among the inputs that
// doesn't "cancel out".) Here "InputIterator" is an iterator over AddInputs.
//
// On failure, some subset of the entries will have been added to the
// BandingStorage, so best considered to be in an indeterminate state.
//
template <typename BandingStorage, typename BandingHasher,
          typename InputIterator>
bool BandingAddRange(BandingStorage *bs, const BandingHasher &bh,
                     InputIterator begin, InputIterator end) {
  using Index = typename BandingStorage::Index;
  struct NoopBacktrackStorage {
    bool UseBacktrack() { return false; }
    void BacktrackPut(Index, Index) {}
    Index BacktrackGet(Index) {
      assert(false);
      return 0;
    }
  } nbts;
  return BandingAddRange(bs, &nbts, bh, begin, end);
}

// ######################################################################
// ######################### Solution Storage ###########################

// Back-substitution and query algorithms unfortunately depend on some
// details of data layout in the final data structure ("solution"). Thus,
// there is no common SolutionStorage covering all the reasonable
// possibilities.

// ###################### SimpleSolutionStorage #########################

// SimpleSolutionStorage is for a row-major storage, typically with no
// unused bits in each ResultRow. This is mostly for demonstration
// purposes as the simplest solution storage scheme. It is relatively slow
// for filter queries.

// concept SimpleSolutionStorage extends RibbonTypes {
//   void PrepareForNumStarts(Index num_starts) const;
//   Index GetNumStarts() const;
//   ResultRow Load(Index slot_num) const;
//   void Store(Index slot_num, ResultRow data);
// };

// Back-substitution for generating a solution from BandingStorage to
// SimpleSolutionStorage.
template <typename SimpleSolutionStorage, typename BandingStorage>
void SimpleBackSubst(SimpleSolutionStorage *sss, const BandingStorage &ss) {
  using CoeffRow = typename BandingStorage::CoeffRow;
  using Index = typename BandingStorage::Index;
  using ResultRow = typename BandingStorage::ResultRow;

  constexpr auto kCoeffBits = static_cast<Index>(sizeof(CoeffRow) * 8U);
  constexpr auto kResultBits = static_cast<Index>(sizeof(ResultRow) * 8U);

  // A column-major buffer of the solution matrix, containing enough
  // recently-computed solution data to compute the next solution row
  // (based also on banding data).
  std::array<CoeffRow, kResultBits> state;
  state.fill(0);

  const Index num_starts = ss.GetNumStarts();
  sss->PrepareForNumStarts(num_starts);
  const Index num_slots = num_starts + kCoeffBits - 1;

  for (Index i = num_slots; i > 0;) {
    --i;
    CoeffRow cr = *const_cast<BandingStorage &>(ss).CoeffRowPtr(i);
    ResultRow rr = *const_cast<BandingStorage &>(ss).ResultRowPtr(i);
    // solution row
    ResultRow sr = 0;
    for (Index j = 0; j < kResultBits; ++j) {
      // Compute next solution bit at row i, column j (see derivation below)
      CoeffRow tmp = state[j] << 1;
      bool bit = (BitParity(tmp & cr) ^ ((rr >> j) & 1)) != 0;
      tmp |= bit ? CoeffRow{1} : CoeffRow{0};

      // Now tmp is solution at column j from row i for next kCoeffBits
      // more rows. Thus, for valid solution, the dot product of the
      // solution column with the coefficient row has to equal the result
      // at that column,
      //   BitParity(tmp & cr) == ((rr >> j) & 1)

      // Update state.
      state[j] = tmp;
      // add to solution row
      sr |= (bit ? ResultRow{1} : ResultRow{0}) << j;
    }
    sss->Store(i, sr);
  }
}

// Common functionality for querying a key (already hashed) in
// SimpleSolutionStorage.
template <typename SimpleSolutionStorage>
typename SimpleSolutionStorage::ResultRow SimpleQueryHelper(
    typename SimpleSolutionStorage::Index start_slot,
    typename SimpleSolutionStorage::CoeffRow cr,
    const SimpleSolutionStorage &sss) {
  using CoeffRow = typename SimpleSolutionStorage::CoeffRow;
  using ResultRow = typename SimpleSolutionStorage::ResultRow;

  constexpr unsigned kCoeffBits = static_cast<unsigned>(sizeof(CoeffRow) * 8U);

  ResultRow result = 0;
  for (unsigned i = 0; i < kCoeffBits; ++i) {
    if (static_cast<unsigned>(cr >> i) & 1U) {
      result ^= sss.Load(start_slot + i);
    }
  }
  return result;
}

// General PHSF query a key from SimpleSolutionStorage.
template <typename SimpleSolutionStorage, typename PhsfQueryHasher>
typename SimpleSolutionStorage::ResultRow SimplePhsfQuery(
    const typename PhsfQueryHasher::Key &key, const PhsfQueryHasher &hasher,
    const SimpleSolutionStorage &sss) {
  const typename PhsfQueryHasher::Hash hash = hasher.GetHash(key);

  return SimpleQueryHelper(hasher.GetStart(hash, sss.GetNumStarts()),
                           hasher.GetCoeffRow(hash), sss);
}

// Filter query a key from SimpleSolutionStorage.
template <typename SimpleSolutionStorage, typename FilterQueryHasher>
bool SimpleFilterQuery(const typename FilterQueryHasher::Key &key,
                       const FilterQueryHasher &hasher,
                       const SimpleSolutionStorage &sss) {
  const typename FilterQueryHasher::Hash hash = hasher.GetHash(key);
  const typename SimpleSolutionStorage::ResultRow expected =
      hasher.GetResultRowFromHash(hash);

  return expected ==
         SimpleQueryHelper(hasher.GetStart(hash, sss.GetNumStarts()),
                           hasher.GetCoeffRow(hash), sss);
}

// #################### InterleavedSolutionStorage ######################

// InterleavedSolutionStorage is row-major at a high level, for good
// locality, and column-major at a low level, for CPU efficiency
// especially in filter querys or relatively small number of result bits
// (== solution columns). The storage is a sequence of "blocks" where a
// block has one CoeffRow for each solution column.

// concept InterleavedSolutionStorage extends RibbonTypes {
//   Index GetNumColumns() const;
//   Index GetNumStarts() const;
//   CoeffRow Load(Index block_num, Index column) const;
//   void Store(Index block_num, Index column, CoeffRow data);
// };

// TODO: not yet implemented here (only in prototype code elsewhere)

}  // namespace ribbon

}  // namespace ROCKSDB_NAMESPACE
