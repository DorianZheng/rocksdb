#include "utilities/titandb/blob_gc.h"

#include "utilities/titandb/version.h"

namespace rocksdb {
namespace titandb {

BlobGC::BlobGC(std::vector<BlobFileMeta*>&& blob_files,
               TitanCFOptions&& _titan_cf_options)
    : inputs_(std::move(blob_files)),
      titan_cf_options_(std::move(_titan_cf_options)) {
  MarkFilesBeingGC(true);
}

BlobGC::~BlobGC() {
  if (current_ != nullptr) {
    current_->Unref();
  }
}

void BlobGC::SetInputVersion(ColumnFamilyHandle* cfh, Version* version) {
  cfh_ = cfh;
  current_ = version;

  current_->Ref();
}

ColumnFamilyData* BlobGC::GetColumnFamilyData() {
  auto* cfhi = reinterpret_cast<ColumnFamilyHandleImpl*>(cfh_);
  return cfhi->cfd();
}

void BlobGC::MarkFilesBeingGC(bool flag) {
  for (auto& f : inputs_) {
    assert(flag == !f->being_gc.load(std::memory_order_relaxed));
    f->being_gc.store(flag, std::memory_order_relaxed);
  }

  for (auto& f : outputs_) {
    assert(flag == !f->being_gc.load(std::memory_order_relaxed));
    f->being_gc.store(flag, std::memory_order_relaxed);
  }
}

void BlobGC::AddOutputFile(BlobFileMeta* blob_file) {
  assert(blob_file->being_gc.load(std::memory_order_relaxed));
  outputs_.push_back(blob_file);
}

void BlobGC::ReleaseGcFiles() {
  MarkFilesBeingGC(false);
}

}  // namespace titandb
}  // namespace rocksdb
