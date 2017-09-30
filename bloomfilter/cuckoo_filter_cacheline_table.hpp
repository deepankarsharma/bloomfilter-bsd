#pragma once

#include <cstdlib>
#include <cstring>

#include <dtl/dtl.hpp>
#include <dtl/hash.hpp>
#include <dtl/math.hpp>

#include <dtl/bloomfilter/bloomfilter_addressing_logic.hpp>
#include <dtl/bloomfilter/cuckoo_filter_helper.hpp>

namespace dtl {


struct cuckoo_filter_cacheline_table;

namespace internal {

template<uint32_t _bucket_cnt_per_word>
bool _find_tag_in_buckets(const cuckoo_filter_cacheline_table& table,
                          uint32_t bucket_idx, uint32_t alternative_bucket_idx, uint32_t tag);
template<>
bool _find_tag_in_buckets<1>(const cuckoo_filter_cacheline_table& table,
                             uint32_t bucket_idx, uint32_t alternative_bucket_idx, uint32_t tag);

} // namespace internal


/// A statically sized Cuckoo filter.
/// The table has the following restrictions/properties:
///   - the bucket must not exceed the size of a processor word
///   - buckets are aligned, so that a single bucket cannot wrap around word boundaries
struct cuckoo_filter_cacheline_table {

  using word_t = uint64_t;
  static constexpr uint32_t word_size_bits = sizeof(word_t) * 8;

  static constexpr uint32_t cacheline_size_bytes = 64;
  static constexpr uint32_t tag_size_bits = 16;
  static constexpr uint32_t tags_per_bucket = word_size_bits / tag_size_bits;

  static constexpr uint32_t tag_mask = (1u << tag_size_bits) - 1;
  static constexpr uint32_t bucket_size_bits = tag_size_bits * tags_per_bucket;
  static_assert(bucket_size_bits <= (sizeof(word_t)*8), "The bucket size must not exceed a word.");

  static constexpr uint32_t word_cnt = cacheline_size_bytes / sizeof(word_t);
  static constexpr uint32_t word_cnt_log2 = dtl::ct::log_2_u32<word_cnt>::value;
  static constexpr uint32_t bucket_cnt_per_word = word_size_bits / bucket_size_bits;

  static constexpr word_t bucket_mask = (bucket_size_bits == word_size_bits) ? word_t(-1) : (word_t(1) << bucket_size_bits) - 1;
  static constexpr uint32_t bucket_count = bucket_cnt_per_word * word_cnt;
  static constexpr uint32_t bucket_addressing_bits = dtl::ct::log_2_u32<dtl::next_power_of_two(bucket_count)>::value;

  static constexpr uint32_t capacity = bucket_count * tags_per_bucket;
  static constexpr uint32_t required_hash_bits = tag_size_bits + bucket_addressing_bits;
  static_assert(required_hash_bits <= 32, "The required hash bits must not exceed 32 bits.");

  static constexpr uint32_t null_tag = 0;
  static constexpr uint32_t overflow_tag = uint32_t(-1);
  static constexpr word_t overflow_bucket = bucket_mask;


  //===----------------------------------------------------------------------===//
  // Members
  //===----------------------------------------------------------------------===//
  alignas(cacheline_size_bytes)
  word_t filter[word_cnt];
  //===----------------------------------------------------------------------===//


  /// C'tor
  cuckoo_filter_cacheline_table() {
    std::memset(&filter[0], 0, word_cnt * sizeof(word_t));
  }


  __forceinline__
  word_t
  read_bucket(const uint32_t bucket_idx) const {
    const auto word_idx = bucket_idx & ((1u << word_cnt_log2) - 1);
    word_t word = filter[word_idx];
    const auto in_word_bucket_idx = bucket_idx >> word_cnt_log2;
    const auto bucket = word >> (bucket_size_bits * in_word_bucket_idx);
    return bucket;
  }


  __forceinline__
  void
  write_bucket(const uint32_t bucket_idx, const word_t bucket_content) {
    const auto to_write = word_t(read_bucket(bucket_idx) ^ bucket_content);
    const auto word_idx = bucket_idx & ((1u << word_cnt_log2) - 1);
    word_t word = filter[word_idx];
    const auto in_word_bucket_idx = bucket_idx >> word_cnt_log2;
    word ^= to_write << (bucket_size_bits * in_word_bucket_idx);
    filter[word_idx] = word;
  }


  __forceinline__
  void
  overflow(const uint32_t bucket_idx) {
    write_bucket(bucket_idx, overflow_bucket);
  }


  __forceinline__
  uint32_t
  read_tag_from_bucket(const word_t bucket, const uint32_t tag_idx) const {
    auto tag = (bucket >> (tag_size_bits * tag_idx)) & tag_mask;
    return static_cast<uint32_t>(tag);
  }


  __forceinline__
  uint32_t
  read_tag(const uint32_t bucket_idx, const uint32_t tag_idx) const {
    auto bucket = read_bucket(bucket_idx);
    auto tag = read_tag_from_bucket(bucket, tag_idx);
    return static_cast<uint32_t>(tag);
  }


  __forceinline__
  uint32_t
  write_tag(const uint32_t bucket_idx, const uint32_t tag_idx, const uint32_t tag_content) {
    auto bucket = read_bucket(bucket_idx);
    auto existing_tag = read_tag(bucket_idx, tag_idx);
    const auto to_write = existing_tag ^ tag_content;
    bucket ^= to_write << (tag_size_bits * tag_idx);
    write_bucket(bucket_idx, bucket);
    return existing_tag;
  }


  __forceinline__
  uint32_t
  insert_tag_kick_out(const uint32_t bucket_idx, const uint32_t tag) {
    // Check whether this is an overflow bucket.
    auto bucket = read_bucket(bucket_idx);
    if (bucket == overflow_bucket) {
      return overflow_tag;
    }
    // Check the buckets' entries for free space.
    for (uint32_t tag_idx = 0; tag_idx < tags_per_bucket; tag_idx++) {
      auto t = read_tag_from_bucket(bucket, tag_idx);
      if (t == tag) {
        return null_tag;
      }
      else if (t == 0) {
        write_tag(bucket_idx, tag_idx, tag);
        return null_tag;
      }
    }
    // couldn't find an empty place
    // kick out existing tag
    uint32_t rnd_tag_idx = static_cast<uint32_t>(std::rand()) % tags_per_bucket;
    return write_tag(bucket_idx, rnd_tag_idx, tag);
  }


  __forceinline__
  uint32_t
  insert_tag(const uint32_t bucket_idx, const uint32_t tag) {
    // Check whether this is an overflow bucket.
    auto bucket = read_bucket(bucket_idx);
    if (bucket == overflow_bucket) {
      return overflow_tag;
    }
    // Check the buckets' entries for free space.
    for (uint32_t tag_idx = 0; tag_idx < tags_per_bucket; tag_idx++) {
      auto t = read_tag_from_bucket(bucket, tag_idx);
      if (t == tag) {
        std::cout << "d";
        return null_tag;
      }
      else if (t == 0) {
        write_tag(bucket_idx, tag_idx, tag);
        return null_tag;
      }
    }
    return tag;
  }


  __forceinline__
  bool
  find_tag_in_buckets(const uint32_t bucket_idx, const uint32_t alternative_bucket_idx, const uint32_t tag) const {
    return internal::_find_tag_in_buckets<bucket_cnt_per_word>(*this, bucket_idx, alternative_bucket_idx, tag);
  }


};


namespace internal {


// General case, where both candidate buckets need to be checked one after another.
template<>
__forceinline__
bool
_find_tag_in_buckets<1>(const cuckoo_filter_cacheline_table& table,
                        const uint32_t bucket_idx, const uint32_t alternative_bucket_idx, const uint32_t tag) {
  using table_t = cuckoo_filter_cacheline_table;
  const auto bucket = table.read_bucket(bucket_idx);
  const auto alternative_bucket = table.read_bucket(alternative_bucket_idx);

  bool found;
  found = packed_value<table_t::word_t, table_t::tag_size_bits>::contains(bucket, tag);
  found |= bucket == table_t::overflow_bucket;
  found |= packed_value<table_t::word_t, table_t::tag_size_bits>::contains(alternative_bucket, tag);
  found |= alternative_bucket == table_t::overflow_bucket;
  return found;
}


// Optimized for the case, where at least two buckets fit into a single word.
template<uint32_t _bucket_cnt_per_word>
__forceinline__
bool
_find_tag_in_buckets(const cuckoo_filter_cacheline_table& table,
                     const uint32_t bucket_idx, const uint32_t alternative_bucket_idx, const uint32_t tag) {
  using table_t = cuckoo_filter_cacheline_table;
  const auto bucket = table.read_bucket(bucket_idx);
  const auto alternative_bucket = table.read_bucket(alternative_bucket_idx);
  bool found = false;
    found |= bucket == table_t::overflow_bucket;
    found |= alternative_bucket == table_t::overflow_bucket;
  // load both words and merge them into one word -> do only one search
  const table_t::word_t merged_buckets = (bucket << table_t::bucket_size_bits) | alternative_bucket;
  found |= packed_value<table_t::word_t, table_t::tag_size_bits>::contains(merged_buckets, tag);
  return found;
}


} // namespace internal



} // namespace dtl
