#include "utilities/titandb/blob_gc.h"
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <cinttypes>
#include "util/string_util.h"
#include "utilities/titandb/version.h"

namespace rocksdb {
namespace titandb {

namespace {
int BlobFileSummary(const std::vector<BlobFileMeta*>& files, char* output,
                    int len) {
  *output = '\0';
  int write = 0;
  for (size_t i = 0; i < files.size(); i++) {
    int sz = len - write;
    int ret;
    char sztxt[16];
    AppendHumanBytes(files.at(i)->file_size(), sztxt, 16);
    ret = snprintf(output + write, sz, "%" PRIu64 "(%s,%f) ",
                   files.at(i)->file_number(), sztxt,
                   files.at(i)->GetDiscardableRatio());
    if (ret < 0 || ret >= sz) break;
    write += ret;
  }
  // if files.size() is non-zero, overwrite the last space
  return write - !files.empty();
}
}  // namespace

BlobGc::BlobGc(std::vector<BlobFileMeta*>&& blob_files,
               TitanCFOptions&& _titan_cf_options)
    : inputs_(std::move(blob_files)),
      titan_cf_options_(std::move(_titan_cf_options)) {
  MarkFilesBeingGC();
}

BlobGc::~BlobGc() {
  if (current_ != nullptr) {
    current_->Unref();
  }
}

void BlobGc::SetInputVersion(ColumnFamilyHandle* cfh, Version* version) {
  cfh_ = cfh;
  current_ = version;

  current_->Ref();
}

ColumnFamilyData* BlobGc::GetColumnFamilyData() {
  return reinterpret_cast<ColumnFamilyHandleImpl*>(cfh_)->cfd();
}

void BlobGc::AddOutputFile(BlobFileMeta* blob_file) {
  blob_file->FileStateTransit(BlobFileMeta::FileEvent::kGCOutput);
  outputs_.push_back(blob_file);
}

void BlobGc::MarkFilesBeingGC() {
  for (auto& f : inputs_) {
    f->FileStateTransit(BlobFileMeta::FileEvent::kGCBegin);
  }
}

void BlobGc::ReleaseGcFiles() {
  for (auto& f : inputs_) {
    f->FileStateTransit(BlobFileMeta::FileEvent::kGCCompleted);
  }

  for (auto& f : outputs_) {
    f->FileStateTransit(BlobFileMeta::FileEvent::kGCCompleted);
  }
}

void BlobGc::InputSummary(char* output, int len) {
  int write =
      snprintf(output, len, "Base version %" PRIu64 ", Raw Inputs: %lu [",
               current_->version_number(), inputs_.size());
  if (write < 0 || write >= len) {
    return;
  }

  write += BlobFileSummary(inputs_, output + write, len - write);
  if (write < 0 || write >= len) {
    return;
  }

  snprintf(output + write, len - write, "]");
}

void BlobGc::OutputSummary(char* output, int len) {
  int write =
      snprintf(output, len, "Base version %" PRIu64 ", Sampled Inputs: %lu [",
               current_->version_number(), sampled_inputs_.size());
  if (write < 0 || write >= len) {
    return;
  }

  write += BlobFileSummary(sampled_inputs_, output + write, len - write);
  if (write < 0 || write >= len) {
    return;
  }

  snprintf(output + write, len - write, "], Outputs: %lu [", outputs_.size());
  if (write < 0 || write >= len) {
    return;
  }

  write += BlobFileSummary(sampled_inputs_, output + write, len - write);
  if (write < 0 || write >= len) {
    return;
  }

  snprintf(output + write, len - write, "]");
}

}  // namespace titandb
}  // namespace rocksdb
