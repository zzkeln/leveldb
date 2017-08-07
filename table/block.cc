// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Decodes the blocks generated by block_builder.cc.

#include "table/block.h"

#include <vector>
#include <algorithm>
#include "leveldb/comparator.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

//一个DataBlock中，最后4字节是放num_restarts的，拿出最后4字节，解析成整数返回。
inline uint32_t Block::NumRestarts() const {
  assert(size_ >= sizeof(uint32_t)); 
  return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents)
    : data_(contents.data.data()),
      size_(contents.data.size()),
      owned_(contents.heap_allocated) {
  if (size_ < sizeof(uint32_t)) { // 一个block至少有一个重启点所占的字节数
    size_ = 0;  // Error marker
  } else {
    size_t max_restarts_allowed = (size_-sizeof(uint32_t)) / sizeof(uint32_t); //除去最后一个restart个数的4字节，当前data block有多少个4字节
    // 如果当前num_restarts大于最多可以允许的个数，那么说明当前size有点问题，
    if (NumRestarts() > max_restarts_allowed) {
      // The size is too small for NumRestarts()
      size_ = 0;
    } else {
      //如果了解一个data block的布局，就知道重启点为啥是如下计算了。
      //第一个重启点的位置
      restart_offset_ = size_ - (1 + NumRestarts()) * sizeof(uint32_t); 
    }
  }
}
  
//data已经由这个Block来托管了.在析构函数里面会释放data_ 
//如果是用户自己负责的内存，那么析构时需要删除这块内存
Block::~Block() {
  if (owned_) {
    delete[] data_;
  }
}

// Helper routine: decode the next block entry starting at "p",
// storing the number of shared key bytes, non_shared key bytes,
// and the length of the value in "*shared", "*non_shared", and
// "*value_length", respectively.  Will not dereference past "limit".
//
// If any errors are detected, returns NULL.  Otherwise, returns a
// pointer to the key delta (just past the three decoded values).
//从头部decode出shared,non_shared以及value_length.这里面为了加快判断的话有一个技巧.返回的是下一个要读取的地址. 
static inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared,
                                      uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return NULL;
  *shared = reinterpret_cast<const unsigned char*>(p)[0];
  *non_shared = reinterpret_cast<const unsigned char*>(p)[1];
  *value_length = reinterpret_cast<const unsigned char*>(p)[2];
  // 至少存在3个字节.但是如果任意3个字节<128的话，表示每个部分都是1个字节.
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {// 如果没有这个fast path的话那么分别取出3个变长uint32.
    if ((p = GetVarint32Ptr(p, limit, shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == NULL) return NULL;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return NULL;
  }
  return p;
}

class Block::Iter : public Iterator {
 private:
  const Comparator* const comparator_; //比较器
  const char* const data_;      // underlying block contents  data block的内容
  uint32_t const restarts_;     // Offset of restart array (list of fixed32) // 重启点数组的首地址
  uint32_t const num_restarts_; // Number of uint32_t entries in restart array //重启点个数
  //以上参数由Block传入，赋值后就不再变化
  
  // current_ is offset in data_ of current entry.  >= restarts_ if !Valid
  //// current_ 指向正在读取的记录的偏移
  uint32_t current_; //当前记录偏移
  uint32_t restart_index_;  // Index of restart block in which current_ falls // 当前记录所在重启点区域
  std::string key_; //上条记录的键值，需要保存起来，因为是prefix-compressed，可能当前记录和上条记录有shared part
  Slice value_; //上条记录的值
  Status status_; //当前迭代器的状态
  inline int Compare(const Slice& a, const Slice& b) const {
    return comparator_->Compare(a, b);
  }

  // Return the offset in data_ just past the end of the current entry.
  inline uint32_t NextEntryOffset() const {
    return (value_.data() + value_.size()) - data_;
  }

  //找到index restart，将它的4字节解析出来，就是记录对应的offset
  uint32_t GetRestartPoint(uint32_t index) {
    assert(index < num_restarts_);
    //数据首地址+restarts的offset+当前第几个restart point
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();//一开始key_为空
    restart_index_ = index;//设置当前所在record的重启点的所以，如果index=0那么就是0即第一个记录
    // current_ will be fixed by ParseNextKey();

    // ParseNextKey() starts at the end of value_, so set value_ accordingly
    uint32_t offset = GetRestartPoint(index);//找到这个索引重启点的偏移量
    value_ = Slice(data_ + offset, 0);//value_指向第index个restart对应的记录的首地址
  }

 public:
  Iter(const Comparator* comparator,
       const char* data,
       uint32_t restarts,
       uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts_),//当前记录指向重启点数组的首位置，表示无效
        restart_index_(num_restarts_) {//当前记录所在重启点索引=重启点个数，表示无效
    assert(num_restarts_ > 0);
  }

  virtual bool Valid() const { return current_ < restarts_; }
  virtual Status status() const { return status_; }
  virtual Slice key() const {
    assert(Valid());
    return key_;
  }
  virtual Slice value() const {
    assert(Valid());
    return value_;
  }

  //Next底层调用了ParseNextKey
  virtual void Next() {
    assert(Valid());
    ParseNextKey();
  }

  virtual void Prev() {
    assert(Valid());

    // Scan backwards to a restart point before current_
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {// 首先找到restart range.
      if (restart_index_ == 0) {
        // No more entries
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);// 然后跳到这个restart range.
    do {
      // Loop until end of current entry hits the start of original entry
    } while (ParseNextKey() && NextEntryOffset() < original);// 在这个restart range里面遍历.
  }

  //Seek非常简单，首先在restart point地方因为里面存放都是有序的完整的key.那么可以restart point 这些地方进行二分查找.
  //然后在restart range里面通过遍历查找.还算是比较高效吧。注意这里如果没有找到的话， 返回的是第一个>=target的对象. 
  virtual void Seek(const Slice& target) {
    // Binary search in restart array to find the last restart point
    // with a key < target
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {// 在restart这些部分二分查找.
      uint32_t mid = (left + right + 1) / 2;
      uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr = DecodeEntry(data_ + region_offset,
                                        data_ + restarts_,
                                        &shared, &non_shared, &value_length);
      if (key_ptr == NULL || (shared != 0)) {
        CorruptionError();
        return;
      }
      Slice mid_key(key_ptr, non_shared);
      if (Compare(mid_key, target) < 0) {
        // Key at "mid" is smaller than "target".  Therefore all
        // blocks before "mid" are uninteresting.
        left = mid;
      } else {
        // Key at "mid" is >= "target".  Therefore all blocks at or
        // after "mid" are uninteresting.
        right = mid - 1;
      }
    }

    // Linear search (within restart block) for first key >= target
    SeekToRestartPoint(left);
    while (true) {// 从这个部分开始遍历查找.
      if (!ParseNextKey()) {
        return;
      }
      if (Compare(key_, target) >= 0) {
        return;
      }
    }
  }

  //构造出一个迭代器之后，首先要将迭代器的当前记录指向首位置，当前记录所在索引赋值为0
  /// 然后解析下一个元素即可.
  virtual void SeekToFirst() {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  virtual void SeekToLast() {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
      // Keep skipping
    }
  }

 private:
  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_.clear();
  }

  //解析当前记录
  bool ParseNextKey() {
    current_ = NextEntryOffset();//将要被解析记录的偏移量，一开始时，就是0
    const char* p = data_ + current_;//p指向将要被解析的记录首位置
    const char* limit = data_ + restarts_;  // Restarts come right after data // 记录指针最大值为重启点数组首位置
    if (p >= limit) { //如果达到结尾的话
      // No more entries to return.  Mark as invalid.
      // 表示迭代到data block最后一条记录结束
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }

    // Decode next entry
    //解析出当前记录
    uint32_t shared, non_shared, value_length;
    ////解析当前记录的共享长度，非共享长度，值的长度，然后返回的指针p指向非共享内容的首位置
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length); // 从p解析key出来.
    if (p == NULL || key_.size() < shared) {
      CorruptionError();
      return false;
    } else {
      /*   
    当为第一条记录时，共享部分为0， key_.append(p, non_shared);这行代码就是将第一条完整记录加在key_后面。
    当不是第一条记录时，此时key_.resize(shared);这行代码获取当前记录和上一条记录共享部分， 
    key_.append(p, non_shared);这行代码就获得非共享部分，凑成完整的记录。
      */
      
      key_.resize(shared);//第一条记录共享长度为0，取出共享的部分
      key_.append(p, non_shared);//第一条记录的非共享内容就是第一条完整记录，取出非共享的部分拼接成完整的key
      value_ = Slice(p + non_shared, value_length);//解析出value值
      while (restart_index_ + 1 < num_restarts_ &&// 判断下一个restart point是否>=current_.
             GetRestartPoint(restart_index_ + 1) < current_) {
        ++restart_index_;//更新记录所在的重启点索引 // 否则需要进入下一个restart point.
      }
      return true;
    }
  }
};

// 采用工厂方法创建BlockIterator.
Iterator* Block::NewIterator(const Comparator* cmp) {
  if (size_ < sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    return NewEmptyIterator();
  } else {
    return new Iter(cmp, data_, restart_offset_, num_restarts);
  }
}

}  // namespace leveldb
