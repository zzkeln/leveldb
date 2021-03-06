// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DBFORMAT_H_
#define STORAGE_LEVELDB_DB_DBFORMAT_H_

#include <stdio.h>
#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/slice.h"
#include "leveldb/table_builder.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

// Grouping of constants.  We may want to make some of these
// parameters set via options.
namespace config {
static const int kNumLevels = 7;

// Level-0 compaction is started when we hit this many files.
static const int kL0_CompactionTrigger = 4;

// Soft limit on number of level-0 files.  We slow down writes at this point.
static const int kL0_SlowdownWritesTrigger = 8;

// Maximum number of level-0 files.  We stop writes at this point.
static const int kL0_StopWritesTrigger = 12;

// Maximum level to which a new compacted memtable is pushed if it
// does not create overlap.  We try to push to level 2 to avoid the
// relatively expensive level 0=>1 compactions and to avoid some
// expensive manifest file operations.  We do not push all the way to
// the largest level since that can generate a lot of wasted disk
// space if the same key space is being repeatedly overwritten.
static const int kMaxMemCompactLevel = 2;

// Approximate gap in bytes between samples of data read during iteration.
static const int kReadBytesPeriod = 1048576;

}  // namespace config

class InternalKey;

// Value types encoded as the last component of internal keys.
// DO NOT CHANGE THESE ENUM VALUES: they are embedded in the on-disk
// data structures.
//存在于InternalKey的key的类型：普通类型和删除类型
enum ValueType {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1
};
// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a ParsedInternalKey object for seeking to a particular
// sequence number (since we sort sequence numbers in decreasing order
// and the value type is embedded as the low 8 bits in the sequence
// number in internal keys, we need to use the highest-numbered
// ValueType, not the lowest).
static const ValueType kValueTypeForSeek = kTypeValue;

//也存在于InternalKey的内部，表示这个key的序号，做snapshot用的。8个字节，其中高位7个字节用于存放序号，最后1个字节用来放ValueType
//这样序号和类型都放在8字节中
typedef uint64_t SequenceNumber;

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
static const SequenceNumber kMaxSequenceNumber =
    ((0x1ull << 56) - 1);

//是InternalKey解析后的表示，包括用户key, 序号和类型（普通类型，删除类型）
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() { }  // Intentionally left uninitialized (for speed) 默认构造函数是空的，啥都没有
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) { }
  std::string DebugString() const;
};

// Return the length of the encoding of "key".
//给一个ParsedInternalKey，那么编码成InternalKey就是用user_key的大小加上8个字节来序列化sequence number和value_type
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

// Append the serialization of "key" to *result.
//将ParsedInternalKey序列化成二进制格式放入result
extern void AppendInternalKey(std::string* result,
                              const ParsedInternalKey& key);

// Attempt to parse an internal key from "internal_key".  On success,
// stores the parsed data in "*result", and returns true.
//
// On error, returns false, leaves "*result" in an undefined state.
//首先将internal_key解析成ParsedInternalKey即反序列化出user_key, sequence_num和value_type
//如果解析正常返回true；如果解析出的数据有异常：例如result没有8个字节或者value_type解析
//出的类型不合法（既不是普通类型也不是删除类型那么非法）那么返回false
//即将那个internal_key解析成ParsedInternalKey中
extern bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result);

// Returns the user key portion of an internal key.
//internal_key中包含sequence_number和value_type，去掉末尾的8字节，返回user_key。
//从internal_key中取出user_key
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

//从internal_key的最后8个字节解析成uint64_t，然后取低8位
//从internal_key中取出value_type，即取出最后1个字节
inline ValueType ExtractValueType(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  const size_t n = internal_key.size();
  //将二进制数据的最后8个字节解析成uint64_t，用的是固定8字节编码
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  //再取8字节的低8位，就是value_type，取最低8位
  unsigned char c = num & 0xff;
  return static_cast<ValueType>(c);
}

// A comparator for internal keys that uses a specified comparator for
// the user key portion and breaks ties by decreasing sequence number.
//基于user-key的comparator提供的internal-key的comparator
class InternalKeyComparator : public Comparator {
 private:
  const Comparator* user_comparator_;//user_key comparator
 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) { }
  virtual const char* Name() const;
  virtual int Compare(const Slice& a, const Slice& b) const;//比较两个InternalKey，优先比较user_key，如果相等那么比较sequence number
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const; //找到最短的separator 使得start<=separator<limit
  virtual void FindShortSuccessor(std::string* key) const; //找到最短的successor是的key<=successor

  const Comparator* user_comparator() const { return user_comparator_; }

  int Compare(const InternalKey& a, const InternalKey& b) const;
};

// Filter policy wrapper that converts from internal keys to user keys
//基于user-key的filter-policy提供的internal-key的filter-policy
class InternalFilterPolicy : public FilterPolicy {
 private:
  const FilterPolicy* const user_policy_;
 public:
  explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) { }
  virtual const char* Name() const;
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const;
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const;
};

// Modules in this directory should keep internal keys wrapped inside
// the following class instead of plain strings so that we do not
// incorrectly use string comparisons instead of an InternalKeyComparator.
//InternalKey是将user_key, sequence number和value_type进行序列号后的一段二进制数据。前面放的user_key数据，后面放8字节，
//其中8字节的高7个字节是sequence number，第8个字节是value_type。
class InternalKey {
 private:
  std::string rep_; //保存user_key, seq, value_type序列化后的二进制数据
 public:
  InternalKey() { }   // Leave rep_ as empty to indicate it is invalid
  //将user_key, s, t序列化后保存在rep_中
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }

  //根据Slice进行复制
  void DecodeFrom(const Slice& s) { rep_.assign(s.data(), s.size()); }
  //返回整个二进制数据
  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  //返回user_key（即去掉rep_的末尾8字节，这8字节序列化了sequence number和value type）
  Slice user_key() const { return ExtractUserKey(rep_); }

  //根据ParsedInternalKey进行复制
  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;
};

//基于internalkey comparator的compare,使用参数是slice的比较函数
inline int InternalKeyComparator::Compare(
    const InternalKey& a, const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}

//将internal_key解析成ParsedInternalKey中
inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false; //大小不符合
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);//将末尾8字节解析成uint64_t
  unsigned char c = num & 0xff; //获得uint64_t的低8位，转换成type
  result->sequence = num >> 8; //获得高56位为sequence number
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);//获得user-key
  return (c <= static_cast<unsigned char>(kTypeValue));//value_type是否合法？
}

// A helper class useful for DBImpl::Get()
 //LookupKey作为memtable的key
//LookupKey表示一块内存，放入InternalKey size + InternalKey.起始地址是start_
//前面最多5个字节（用varint 32_t来编码InternalKey的长度）存放InternalKey size；中间是user_key，起始地址是kstart_
//最后8个字节的为序列化的sequence number和标识普通类型的value_type
//[start, kstart-1]表示varint32，[kstart, end-8-1]表示user-key，[end-8, end]表示sequence number和type的uint64
// start_[后面user_key+8字节的大小](最大5个字节)+kstart_(user_key的起始地址)...+(最后8字节为sequence_number和普通类型value_type的序列化)end_
class LookupKey {
 public:
  // Initialize *this for looking up user_key at a snapshot with
  // the specified sequence number.
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  ~LookupKey();

  // Return a key suitable for lookup in a MemTable.
  //返回能够为memtable查询的key
  //LookUpKey整个作为memtable的key
  Slice memtable_key() const { return Slice(start_, end_ - start_); }

  // Return an internal key (suitable for passing to an internal iterator)
  //返回一个InternalKey，从kstart_开始，大小为end_-kstart_
  //这个应该是sstable查询的key
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

  // Return the user key
  //返回user_key，从kstart_开始，大小为end-kstart_-8，其中8表示tag的大小
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  // We construct a char array of the form:
  //    klength  varint32 (最大会占5个字节)              <-- start_：此为整个内存的起始处
  //    userkey  char[klength]          <-- kstart_：此为user_key的起始处
  //    tag      uint64                 <-- end_：整个内存的下一个字节处
  // The array is a suitable MemTable key.
  // The suffix starting with "userkey" can be used as an InternalKey.
  const char* start_;//整个内存的起始地址
  const char* kstart_;//user_key的起始地址
  const char* end_;//整个内存的末尾的下一个字节
  char space_[200];      // Avoid allocation for short keys，如果[start, end]需要的字节数比较少，那么使用这个内存，否则要用new在堆上来分配
  // No copying allowed
  LookupKey(const LookupKey&);
  void operator=(const LookupKey&);
};

inline LookupKey::~LookupKey() {
  //如果没使用space_的内存，那么是用new从堆上分配的，需要delete掉
  if (start_ != space_) delete[] start_;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DBFORMAT_H_
