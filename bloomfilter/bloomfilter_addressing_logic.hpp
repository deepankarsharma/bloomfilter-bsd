#pragma once

#include <bitset>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include <dtl/dtl.hpp>
#include <dtl/div.hpp>
#include <dtl/math.hpp>


namespace dtl {

template<typename Th,    // the hash value type
    typename Tblock      // the block type
>
struct bloomfilter_addressing_logic {

  using block_t = Tblock;
  using size_t = $u32;
  using hash_value_t = Th;


  //===----------------------------------------------------------------------===//
  // Members
  //===----------------------------------------------------------------------===//
  const size_t block_cnt; // the number of blocks
//  const size_t block_cnt_log2; // the number of bits required to address the individual blocks
  const dtl::fast_divisor_u32_t fast_divisor;
  //===----------------------------------------------------------------------===//


  /// Determines the actual block count based on the given bitlength of the Bloom filter.
  /// Note: The actual size of the Bloom filter might be larger than the given 'bitlength'.
  static constexpr
  size_t
  determine_block_cnt(const std::size_t bitlength) {
    u32 desired_block_cnt = (bitlength + (block_t::block_bitlength - 1)) / block_t::block_bitlength;
    u32 actual_block_cnt = dtl::next_cheap_magic(desired_block_cnt).divisor;
    return actual_block_cnt;
  }


 public:

  explicit
  bloomfilter_addressing_logic(const std::size_t length) noexcept
      : block_cnt(determine_block_cnt(length)),
        fast_divisor(dtl::next_cheap_magic(block_cnt)) { }

  bloomfilter_addressing_logic(const bloomfilter_addressing_logic&) noexcept = default;

  bloomfilter_addressing_logic(bloomfilter_addressing_logic&&) noexcept = default;


  __forceinline__ __host__ __device__
  std::size_t
  length() const noexcept {
    return block_cnt * block_t::block_bitlength;
  }


  __forceinline__ __host__ __device__
  std::size_t
  word_cnt() const noexcept {
    return block_cnt * block_t::word_cnt;
  }


  /// Returns the index of the block the hash value maps to.
  __forceinline__ __host__ __device__
  size_t
  get_block_idx(const hash_value_t hash_value) const noexcept {
    const size_t block_idx = dtl::fast_mod_u32(hash_value, fast_divisor);
    return block_idx;
    const size_t word_idx = block_idx * block_t::word_cnt;
  }


  /// Returns the index of the first word of the block the hash value maps to.
  __forceinline__ __host__ __device__
  size_t
  get_word_idx(const hash_value_t hash_value) const noexcept {
    const size_t block_idx = get_block_idx(hash_value);
    const size_t word_idx = block_idx * block_t::word_cnt;
    return word_idx;
  }

};

} // namespace dtl
