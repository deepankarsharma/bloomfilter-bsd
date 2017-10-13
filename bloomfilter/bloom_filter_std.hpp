#pragma once

#include <bitset>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include <dtl/dtl.hpp>
#include <dtl/div.hpp>
#include <dtl/math.hpp>
#include <dtl/mem.hpp>
#include <dtl/simd.hpp>

#include "bloomfilter_addressing_logic.hpp"
#include "hash_family.hpp"

namespace dtl {
namespace bloom_filter {

template<
    typename Tk,           // the key type
//    typename HashFn,       // the hash function (family) to use
    block_addressing AddrMode = block_addressing::POWER_OF_TWO  // the addressing scheme
>
struct bloom_filter_std {

  using key_t = Tk;
  using word_t = uint32_t;

  using HashFn = dtl::hash::dyn::mul32;

  //===----------------------------------------------------------------------===//
  // Inspect the given hash functions
  //===----------------------------------------------------------------------===//

  using hash_value_t = $u32; //decltype(HashFn<key_t>::hash(0)); // TODO find out why NVCC complains
  static_assert(std::is_integral<hash_value_t>::value, "Hash function must return an integral type.");
  static constexpr u32 hash_value_bitlength = sizeof(hash_value_t) * 8;

  //===----------------------------------------------------------------------===//

  // A fake block; required by addressing logic
  struct block_t {
    static constexpr uint32_t block_bitlength = sizeof(word_t) * 8;
    static constexpr uint32_t word_cnt = 1;
  };

  using addr_t = bloomfilter_addressing_logic<AddrMode, hash_value_t, block_t>;
  using size_t = $u64;
//  using size_t = $u32;


  //===----------------------------------------------------------------------===//
  // Members
  //===----------------------------------------------------------------------===//
  /// The addressing scheme.
  const addr_t addr;
  const uint32_t k;
  //===----------------------------------------------------------------------===//


 public:

  explicit
  bloom_filter_std(const std::size_t length, const uint32_t k) noexcept
      : addr(length), k(k) { }

  bloom_filter_std(const bloom_filter_std&) noexcept = default;

  bloom_filter_std(bloom_filter_std&&) noexcept = default;


  /// Returns the size of the Bloom filter (in number of bits).
  __forceinline__ __host__ __device__
  std::size_t
  length() const noexcept {
    return static_cast<std::size_t>(addr.block_cnt) * sizeof(word_t) * 8;
  }


  /// Returns the number of words the Bloom filter consists of.
  __forceinline__ __host__ __device__
  std::size_t
  word_cnt() const noexcept {
    return addr.block_cnt;
  }


  __forceinline__ __host__
  void
  insert(const key_t& key, word_t* __restrict filter) const noexcept {
    constexpr uint32_t word_bitlength = sizeof(word_t) * 8;
    constexpr uint32_t word_bitlength_log2 = dtl::ct::log_2<word_bitlength>::value;
    constexpr word_t word_mask = (word_t(1u) << word_bitlength_log2) - 1;
    const auto addressing_bits = addr.get_required_addressing_bits();

    // Set one bit per word at a time.
    for (uint32_t current_k = 0; current_k < k; current_k++) {
      const hash_value_t hash_val = HashFn::hash(key, current_k);
      const hash_value_t word_idx = addr.get_word_idx(hash_val);
      const hash_value_t bit_idx = (hash_val >> (word_bitlength - addressing_bits)) & word_mask;
      filter[word_idx] |= word_t(1u) << bit_idx;
    }
  }


  __forceinline__
  void
  batch_insert(const key_t* __restrict keys, const uint32_t key_cnt,
               word_t* __restrict filter) const {
    for (uint32_t j = 0; j < key_cnt; j++) {
      insert(keys[j], filter);
    }
  };


  __forceinline__ __host__ __device__
  u1
  contains(const key_t& key, const word_t* __restrict filter) const noexcept {
    constexpr uint32_t word_bitlength = sizeof(word_t) * 8;
    constexpr uint32_t word_bitlength_log2 = dtl::ct::log_2<word_bitlength>::value;
    constexpr word_t word_mask = (word_t(1u) << word_bitlength_log2) - 1;
    const auto addressing_bits = addr.get_required_addressing_bits();

    // Test one bit per word at a time.
    for (uint32_t current_k = 0; current_k < k; current_k++) {
      const hash_value_t hash_val = HashFn::hash(key, current_k);
      const hash_value_t word_idx = addr.get_word_idx(hash_val);
      const hash_value_t bit_idx = (hash_val >> (word_bitlength - addressing_bits)) & word_mask;
      const bool hit = filter[word_idx] & (word_t(1u) << bit_idx);
      if (!hit) return false;
    }
    return true;
  }


  template<typename Tv, typename = std::enable_if_t<dtl::is_vector<Tv>::value>>
  __forceinline__ __host__
  typename dtl::vec<key_t, dtl::vector_length<Tv>::value>::mask_t
  simd_contains(const Tv& keys,
                const word_t* __restrict filter) const noexcept {
    using vec_t = dtl::vec<key_t, dtl::vector_length<Tv>::value>;
    using hash_value_vt = dtl::vec<hash_value_t, dtl::vector_length<Tv>::value>;
    using mask_t = typename vec_t::mask_t;

    constexpr uint32_t word_bitlength = sizeof(word_t) * 8;
    constexpr uint32_t word_bitlength_log2 = dtl::ct::log_2<word_bitlength>::value;
    constexpr uint32_t word_mask = (word_t(1u) << word_bitlength_log2) - 1;
    const auto addressing_bits = addr.get_required_addressing_bits();

    // Test one bit per word at a time.
    const hash_value_vt hash_vals = HashFn::hash(keys, 0);
    const auto word_idxs = addr.get_block_idx(hash_vals);
    const auto bit_idxs = (hash_vals >> (word_bitlength - addressing_bits)) & word_mask;
    const auto lsb_set = vec_t::make(word_t(1u));
    mask_t exec_mask = (dtl::gather(filter, word_idxs) & (lsb_set << bit_idxs)) != 0;

    if (exec_mask.any()) {
      for (uint32_t current_k = 1; current_k < k; current_k++) {
        const vec_t hash_vals = HashFn::hash(keys, current_k);
        const vec_t word_idxs = addr.get_block_idx(hash_vals).zero_mask(exec_mask);
        const auto bit_idxs = (hash_vals >> (word_bitlength - addressing_bits)) & word_mask;
        exec_mask &= (dtl::gather(filter, word_idxs) & (lsb_set << bit_idxs)) != 0;
        if (exec_mask.none()) return !exec_mask;
      }

    }
    return !exec_mask;
  }


  __forceinline__
  uint64_t
  batch_contains(const key_t* __restrict keys, const uint32_t key_cnt,
                 const word_t* __restrict filter,
                 uint32_t* __restrict match_positions, const uint32_t match_offset) const {
    $u32* match_writer = match_positions;
    for (uint32_t j = 0; j < key_cnt; j++) {
      const auto is_contained = contains(keys[j], filter);
      *match_writer = j + match_offset;
      match_writer += is_contained;
    }
    return match_writer - match_positions;
  };

//  /// Performs a batch-probe
//  template<u64 vector_len = dtl::simd::lane_count<key_t>>
//  __forceinline__
//  $u64
//  batch_contains(const key_t* keys, u32 key_cnt,
//                 const word_t* filter,
//                 $u32* match_positions, u32 match_offset) const {
//    const key_t* reader = keys;
//    $u32* match_writer = match_positions;
//
//    // determine the number of keys that need to be probed sequentially, due to alignment
//    u64 required_alignment_bytes = 64;
//    u64 unaligned_key_cnt = dtl::mem::is_aligned(reader)
//                            ? (required_alignment_bytes - (reinterpret_cast<uintptr_t>(reader) % required_alignment_bytes)) / sizeof(key_t)
//                            : key_cnt;
//    // process the unaligned keys sequentially
//    $u64 read_pos = 0;
//    for (; read_pos < unaligned_key_cnt; read_pos++) {
//      u1 is_match = contains(*reader, filter);
//      *match_writer = static_cast<$u32>(read_pos) + match_offset;
//      match_writer += is_match;
//      reader++;
//    }
//    // process the aligned keys vectorized
//    using vec_t = vec<key_t, vector_len>;
//    using mask_t = typename vec<key_t, vector_len>::mask;
//    u64 aligned_key_cnt = ((key_cnt - unaligned_key_cnt) / vector_len) * vector_len;
//    for (; read_pos < (unaligned_key_cnt + aligned_key_cnt); read_pos += vector_len) {
//      assert(dtl::mem::is_aligned(reader, 32));
//      const mask_t mask = simd_contains(*reinterpret_cast<const vec_t*>(reader), filter);
//      u64 match_cnt = mask.to_positions(match_writer, read_pos + match_offset);
//      match_writer += match_cnt;
//      reader += vector_len;
//    }
//    // process remaining keys sequentially
//    for (; read_pos < key_cnt; read_pos++) {
//      u1 is_match = contains(*reader, filter);
//      *match_writer = static_cast<$u32>(read_pos) + match_offset;
//      match_writer += is_match;
//      reader++;
//    }
//    return match_writer - match_positions;
//  }


};

} // namespace bloom_filter
} // namespace dtl
