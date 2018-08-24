// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

//#include "stdio.h"
//extern double durations[];
//int flag=0,flag1=0;

namespace leveldb {

struct Table::Rep {
  ~Rep() {
    delete filter;
    delete [] filter_data;
    delete index_block;
  }

  Options options;
  Status status;
  RandomAccessFile* file;
  uint64_t cache_id;
  FilterBlockReader* filter;
  const char* filter_data;

  BlockHandle metaindex_handle;  // Handle to metaindex_block: saved from footer
  Block* index_block;
};

Status Table::Open(const Options& options,
                   RandomAccessFile* file,
                   uint64_t size,
                   Table** table) {
  *table = NULL;
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // Read the index block
  BlockContents contents;
  Block* index_block = NULL;
  if (s.ok()) {
    ReadOptions opt;
    if (options.paranoid_checks) {
      opt.verify_checksums = true;
    }
	  //printf("table.cc, open, index_handle.size= %dKB,metaindex_handle_.size= %dB\n",footer.index_handle().size()/1024,footer.metaindex_handle().size());

    s = ReadBlock(file, opt, footer.index_handle(), &contents);
    if (s.ok()) {
      index_block = new Block(contents);//组件block
    }
  }

  if (s.ok()) {
    // We've successfully read the footer and the index block: we're
    // ready to serve requests.
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;
    rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
    rep->filter_data = NULL;
    rep->filter = NULL;
    *table = new Table(rep);
    (*table)->ReadMeta(footer);
  } else {
    if (index_block) delete index_block;
  }

  return s;
}

void Table::ReadMeta(const Footer& footer) {
  if (rep_->options.filter_policy == NULL) {
    return;  // Do not need any metadata
  }

  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents contents;
  if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
    // Do not propagate errors since meta info is not needed for operation
    return;
  }
  Block* meta = new Block(contents);

  Iterator* iter = meta->NewIterator(BytewiseComparator());
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  iter->Seek(key);
  if (iter->Valid() && iter->key() == Slice(key)) {
    ReadFilter(iter->value());
  }
  delete iter;
  delete meta;
}

void Table::ReadFilter(const Slice& filter_handle_value) {
  Slice v = filter_handle_value;
  BlockHandle filter_handle;
  if (!filter_handle.DecodeFrom(&v).ok()) {
    return;
  }

  // We might want to unify with ReadBlock() if we start
  // requiring checksum verification in Table::Open.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents block;
  if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
    return;
  }
  if (block.heap_allocated) {
    rep_->filter_data = block.data.data();     // Will need to delete later
  }
  rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}

Table::~Table() {
  delete rep_;
}

static void DeleteBlock(void* arg, void* ignored) {
  delete reinterpret_cast<Block*>(arg);
}

static void DeleteCachedBlock(const Slice& key, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

static void ReleaseBlock(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
Iterator* Table::BlockReader(void* arg,
                             const ReadOptions& options,
                             const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = NULL;
  Cache::Handle* cache_handle = NULL;

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.

  if (s.ok()) {
    BlockContents contents;
    if (block_cache != NULL) {
		//printf("table.cc BlockReader, block_cache != NULL\n");
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer+8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != NULL) {
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
		//printf("table.cc BlockReader, ReadBlock\n");
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache) {
			//printf("table.cc BlockReader, insert cache\n");
            cache_handle = block_cache->Insert(
                key, block, block->size(), &DeleteCachedBlock);
			
          }
		  //printf("table.cc, block reader, block size=%d\n", block->size());
		 
        }
      }
    } else {
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }

  Iterator* iter;
  if (block != NULL) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == NULL) {
      iter->RegisterCleanup(&DeleteBlock, block, NULL);
    } else {
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

Status Table::InternalGet(const ReadOptions& options, const Slice& k,
                          void* arg,
                          void (*saver)(void*, const Slice&, const Slice&)) { 
  Status s;
  //printf("table.cc, internal get, begin==================\n");
   //printf("table.cc before index_block new\n");
//#define s_to_ns 1000000000  
 Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
 
	
    //if(flag1==1) flag=0; 
	//flag=0;
//struct timespec index_seek_begin, index_seek_end;
//clock_gettime(CLOCK_MONOTONIC,&index_seek_begin); 
  iiter->Seek(k);//seek the index block to locate data block. this seek is implmented in block.cc. binary search is used.
//clock_gettime(CLOCK_MONOTONIC,&index_seek_end); 
//durations[1]+=( (int)index_seek_end.tv_sec+((double)index_seek_end.tv_nsec)/s_to_ns ) - ( (int)index_seek_begin.tv_sec+((double)index_seek_begin.tv_nsec)/s_to_ns );
   //flag=0;
//printf("table.cc, internal get, index_seek time=%f ms\n",dur*1000);

  if (iiter->Valid()) {
  


 
    Slice handle_value = iiter->value();//key:offset_:size_
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
	//printf("table.cc, internal get, filter is %d\n",filter != NULL);
    if (filter != NULL &&
        handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
			printf("table.cc, internal get, filter not found++++++++++++\n");
      // Not found
    } else {
	//printf("table.cc before block_iter reader\n");
	
//struct timespec test_begin, test_end;
//clock_gettime(CLOCK_MONOTONIC,&test_begin); 
      Iterator* block_iter = BlockReader(this, options, iiter->value());
//clock_gettime(CLOCK_MONOTONIC,&test_end); 
//durations[9]+=( (int)test_end.tv_sec+((double)test_end.tv_nsec)/s_to_ns ) - ( (int)test_begin.tv_sec+((double)test_begin.tv_nsec)/s_to_ns );


	  //printf("table.cc, internal get, come to blcok_iter->Seek\n");
	  //flag=0;
	 //if(flag1==1) flag=1;
	 
//struct timespec block_seek_begin, block_seek_end;
//clock_gettime(CLOCK_MONOTONIC,&block_seek_begin); 
      block_iter->Seek(k);//seek the block to locate the key
//clock_gettime(CLOCK_MONOTONIC,&block_seek_end); 
//durations[6]+=( (int)block_seek_end.tv_sec+((double)block_seek_end.tv_nsec)/s_to_ns ) - ( (int)block_seek_begin.tv_sec+((double)block_seek_begin.tv_nsec)/s_to_ns );

	  //flag=0;
	  //printf("table.cc, internal get, after blcok_iter->Seek\n");

      if (block_iter->Valid()) {
		//printf("table.cc, internal get, come to saver, k=%s, block_iter->key()=%s\n",k.data(),block_iter->key().data());
        (*saver)(arg, block_iter->key(), block_iter->value());
		//printf("table.cc, internal get, come to after saver\n");
		//printf("table.cc, internal get,tkey=%s, key=%s\n", k.data(),block_iter->key().data());
      }
      s = block_iter->status();
      delete block_iter;
    }
	


  }
  
  if (s.ok()) {
    s = iiter->status();
  }
  delete iiter;
  

  return s;
}


uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key is past the last key in the file.  Approximate the offset
    // by returning the offset of the metaindex block (which is
    // right near the end of the file).
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace leveldb
