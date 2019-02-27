#include "utilities/titandb/blob_gc_job.h"

namespace rocksdb {
namespace titandb {

// Write callback for garbage collection to check if key has been updated
// since last read. Similar to how OptimisticTransaction works.
class BlobGcJob::GcWriteCallback : public WriteCallback {
 public:
  GcWriteCallback(ColumnFamilyHandle* cfh, std::string&& _key,
                  const BlobIndex& index)
      : cfh_(cfh), key_(std::move(_key)), origin_index_(index) {}

  Status Callback(DB* db) override {
    auto* db_impl = reinterpret_cast<DBImpl*>(db);
    PinnableSlice value;
    bool is_blob_index;
    auto s = db_impl->GetImpl(ReadOptions(), cfh_, key_, &value,
                              nullptr /*value_found*/,
                              nullptr /*read_callback*/, &is_blob_index);
    if (!s.ok() && !s.IsNotFound()) {
      // Error.
      assert(!s.IsBusy());
    } else if (s.IsNotFound()) {
      s = Status::Busy("key deleted");
    } else if (!is_blob_index) {
      s = Status::Busy("key overwritten");
    }

    if (s.ok()) {
      BlobIndex cur_index;
      s = cur_index.DecodeFrom(&value);
      if (!s.ok()) {
        assert(!s.IsBusy());
      } else if (cur_index != origin_index_) {
        s = Status::Busy("key overwritten");
      }
    }

    return s;
  }

  bool AllowWriteBatching() override { return false; }

 private:
  ColumnFamilyHandle* cfh_;
  // Key to check
  std::string key_;
  // Original blob index
  BlobIndex origin_index_;
};

BlobGcJob::BlobGcJob(uint64_t job_id, BlobGc* blob_gc, DB* db,
                     port::Mutex* mutex, const TitanDBOptions& titan_db_options,
                     Env* env, const EnvOptions& env_options,
                     BlobFileManager* blob_file_manager,
                     VersionSet* version_set, LogBuffer* log_buffer,
                     std::atomic_bool* shutting_down,
                     BlobGcJobStats* gc_job_stats)
    : job_id_(job_id),
      blob_gc_(blob_gc),
      base_db_(db),
      base_db_impl_(reinterpret_cast<DBImpl*>(base_db_)),
      mutex_(mutex),
      db_options_(titan_db_options),
      env_(env),
      env_options_(env_options),
      blob_file_manager_(blob_file_manager),
      version_set_(version_set),
      shutting_down_(shutting_down),
      log_buffer_(log_buffer),
      gc_job_stats_(gc_job_stats) {}

BlobGcJob::~BlobGcJob() {
  if (cmp_) delete cmp_;
}

Status BlobGcJob::Prepare() {
  start_micros_ = env_->NowMicros();
  return Status::OK();
}

Status BlobGcJob::Run() {
  log_buffer_->FlushBufferToLog();
  LogGcOnStart();

  auto s = Sample();

  if (s.ok()) s = DoRunGC();

  // We have to make sure crash consistency, but LSM db MANIFEST and BLOB db
  // MANIFEST are separate, so we need to make sure all new blob file have
  // added to db before we rewrite any key to LSM
  if (s.ok()) s = InstallOutputs();
  if (s.ok()) s = RewriteToLSM();

  LogGcOnFinish();
  LogFlush(db_options_.info_log.get());

  return s;
}

Status BlobGcJob::Finish() {
  // TODO(@DorianZheng) cal discardable size for new blob file
  auto s = DeleteInputs();
  gc_job_stats_->micros = env_->NowMicros() - start_micros_;
  return s;
}

Status BlobGcJob::Sample() {
  StopWatch sw(env_, nullptr, 0, &gc_job_stats_->sampling_micros);

  auto gc_threshold = blob_gc_->titan_cf_options().blob_file_discardable_ratio;
  auto sample_ratio = blob_gc_->titan_cf_options().sample_file_size_ratio;
  std::vector<BlobFileMeta*> sampled_inputs;

  for (const auto& file : blob_gc_->inputs()) {
    if (file->GetDiscardableRatio() >= gc_threshold) {
      sampled_inputs.emplace_back(file);
      continue;
    }
    std::unique_ptr<RandomAccessFileReader> file_reader;
    const int readahead = 256 << 10;
    auto s = NewFileReader(file->file_number(), readahead, db_options_,
                           env_options_, env_, &file_reader);
    if (!s.ok()) {
      fprintf(stderr, "NewFileReader failed, status:%s\n",
              s.ToString().c_str());
      abort();
    }
    BlobFileIterator iter(std::move(file_reader), file->file_number(),
                          file->file_size(), blob_gc_->titan_cf_options());

    uint64_t sample_size_window =
        static_cast<uint64_t>(file->file_size() * sample_ratio);
    Random64 random64(file->file_size());
    uint64_t sample_begin_offset =
        random64.Uniform(file->file_size() - sample_size_window);

    iter.IterateForPrev(sample_begin_offset);
    // TODO(@DorianZheng) sample_begin_offset maybe out of data block size, need
    // more elegant solution
    if (iter.status().IsInvalidArgument()) {
      iter.IterateForPrev(0);
    }
    if (!iter.status().ok()) {
      fprintf(stderr,
              "IterateForPrev faile, file number[%lu] size[%lu] status[%s]\n",
              static_cast<size_t>(file->file_number()),
              static_cast<size_t>(file->file_size()),
              iter.status().ToString().c_str());
      abort();
    }

    uint64_t iterated_size{0};
    uint64_t discardable_size{0};
    for (iter.Next(); iterated_size < sample_size_window &&
                      iter.status().ok() && iter.Valid();
         iter.Next()) {
      BlobIndex blob_index = iter.GetBlobIndex();
      uint64_t total_length = blob_index.blob_handle.size;
      iterated_size += total_length;
      if (DiscardEntry(iter.key(), blob_index)) {
        discardable_size += total_length;
      }
    }
    assert(iter.status().ok());

    if (discardable_size >= sample_size_window * gc_threshold) {
      sampled_inputs.emplace_back(file);
    }
  }

  if (sampled_inputs.empty())
    return Status::Aborted("No blob file need to be gc");

  blob_gc_->set_sampled_inputs(std::move(sampled_inputs));

  return Status::OK();
}

Status BlobGcJob::DoRunGC() {
  Status s;

  std::unique_ptr<BlobFileMergeIterator> gc_iter;
  s = BuildIterator(&gc_iter);
  if (!s.ok()) return s;
  if (!gc_iter) return Status::Aborted("Build iterator for gc failed");

  std::unique_ptr<BlobFileHandle> blob_file_handle;
  std::unique_ptr<BlobFileBuilder> blob_file_builder;

  auto* cfh = blob_gc_->column_family_handle();

  //  uint64_t drop_entry_num = 0;
  //  uint64_t drop_entry_size = 0;
  //  uint64_t total_entry_num = 0;
  //  uint64_t total_entry_size = 0;

  uint64_t file_size = 0;

  std::string last_key;
  bool last_key_valid = false;
  gc_iter->SeekToFirst();
  assert(gc_iter->Valid());
  for (; gc_iter->Valid(); gc_iter->Next()) {
    if (IsShuttingDown()) {
      s = Status::ShutdownInProgress();
      break;
    }
    BlobIndex blob_index = gc_iter->GetBlobIndex();
    if (!last_key.empty() && !gc_iter->key().compare(last_key)) {
      if (last_key_valid) {
        continue;
      }
    } else {
      last_key = gc_iter->key().ToString();
      last_key_valid = false;
    }

    if (DiscardEntry(gc_iter->key(), blob_index)) {
      continue;
    }

    last_key_valid = true;

    // Rewrite entry to new blob file
    if ((!blob_file_handle && !blob_file_builder) ||
        file_size >= blob_gc_->titan_cf_options().blob_file_target_size) {
      if (file_size >= blob_gc_->titan_cf_options().blob_file_target_size) {
        assert(blob_file_builder);
        assert(blob_file_handle);
        assert(blob_file_builder->status().ok());
        blob_file_build_infos_.emplace_back(BlobFileBuildInfo{
            std::move(blob_file_handle), std::move(blob_file_builder)});
      }
      s = blob_file_manager_->NewFile(&blob_file_handle);
      if (!s.ok()) {
        break;
      }
      blob_file_builder = unique_ptr<BlobFileBuilder>(new BlobFileBuilder(
          blob_gc_->titan_cf_options(), blob_file_handle->GetFile()));
      file_size = 0;
    }
    assert(blob_file_handle);
    assert(blob_file_builder);

    BlobRecord blob_record;
    blob_record.key = gc_iter->key();
    blob_record.value = gc_iter->value();

    //    file_size_ += blob_record.key.size() + blob_record.value.size();

    BlobIndex new_blob_index;
    new_blob_index.file_number = blob_file_handle->GetNumber();
    blob_file_builder->Add(blob_record, &new_blob_index.blob_handle);
    std::string index_entry;
    new_blob_index.EncodeTo(&index_entry);

    // Store WriteBatch for rewriting new Key-Index pairs to LSM
    GcWriteCallback callback(cfh, blob_record.key.ToString(),
                             std::move(blob_index));
    rewrite_batches_.emplace_back(
        std::make_pair(WriteBatch(), std::move(callback)));
    auto& wb = rewrite_batches_.back().first;
    s = WriteBatchInternal::PutBlobIndex(&wb, cfh->GetID(), blob_record.key,
                                         index_entry);
    if (!s.ok()) {
      break;
    }
  }

  if (gc_iter->status().ok() && s.ok()) {
    if (blob_file_builder && blob_file_handle) {
      assert(blob_file_builder->status().ok());
      blob_file_build_infos_.emplace_back(BlobFileBuildInfo{
          std::move(blob_file_handle), std::move(blob_file_builder)});
    } else {
      assert(!blob_file_builder);
      assert(!blob_file_handle);
    }
  } else if (!gc_iter->status().ok()) {
    return gc_iter->status();
  }

  return s;
}

Status BlobGcJob::BuildIterator(unique_ptr<BlobFileMergeIterator>* result) {
  Status s;
  const auto& inputs = blob_gc_->sampled_inputs();
  assert(!inputs.empty());
  std::vector<std::unique_ptr<BlobFileIterator>> list;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    std::unique_ptr<RandomAccessFileReader> file;
    // TODO(@DorianZheng) set read ahead size
    s = NewFileReader(inputs[i]->file_number(), 0, db_options_, env_options_,
                      env_, &file);
    if (!s.ok()) {
      break;
    }
    list.emplace_back(std::unique_ptr<BlobFileIterator>(new BlobFileIterator(
        std::move(file), inputs[i]->file_number(), inputs[i]->file_size(),
        blob_gc_->titan_cf_options())));
  }

  if (s.ok()) result->reset(new BlobFileMergeIterator(std::move(list)));

  return s;
}

bool BlobGcJob::DiscardEntry(const Slice& key, const BlobIndex& blob_index) {
  PinnableSlice index_entry;
  bool is_blob_index;
  auto s = base_db_impl_->GetImpl(
      ReadOptions(), blob_gc_->column_family_handle(), key, &index_entry,
      nullptr /*value_found*/, nullptr /*read_callback*/, &is_blob_index);
  if (!s.ok() && !s.IsNotFound()) {
    fprintf(stderr, "GetImpl err, status:%s\n", s.ToString().c_str());
    abort();
  }
  if (s.IsNotFound() || !is_blob_index) {
    // Either the key is deleted or updated with a newer version which is
    // inlined in LSM.
    return true;
  }

  BlobIndex other_blob_index;
  s = other_blob_index.DecodeFrom(&index_entry);
  if (!s.ok()) {
    abort();
  }

  return !(blob_index == other_blob_index);
}

Status BlobGcJob::InstallOutputs() {
  Status s;

  for (auto& build_info : blob_file_build_infos_) {
    s = build_info.builder->Finish();
    if (!s.ok()) {
      break;
    }
  }

  if (s.ok()) {
    std::vector<BlobFileManager::BlobFilePair> blob_file_pairs;
    for (auto& builder : this->blob_file_build_infos_) {
      auto file = std::make_shared<BlobFileMeta>(
          builder.handle->GetNumber(),
          builder.handle->GetFile()->GetFileSize());
      blob_gc_->AddOutputFile(file.get());
      blob_file_pairs.emplace_back(
          BlobFileManager::BlobFilePair{file, std::move(builder.handle)});
    }
    this->blob_file_manager_->BatchFinishFiles(
        blob_gc_->column_family_handle()->GetID(), blob_file_pairs);
  } else {
    std::vector<unique_ptr<BlobFileHandle>> handles;
    for (auto& builder : this->blob_file_build_infos_)
      handles.emplace_back(std::move(builder.handle));
    this->blob_file_manager_->BatchDeleteFiles(handles);
  }

  return s;
}

Status BlobGcJob::RewriteToLSM() {
  Status s;
  auto* db_impl = reinterpret_cast<DBImpl*>(this->base_db_);

  WriteOptions wo;
  wo.low_pri = true;
  wo.ignore_missing_column_families = true;
  for (auto& write_batch : this->rewrite_batches_) {
    if (blob_gc_->GetColumnFamilyData()->IsDropped()) {
      s = Status::Aborted("Column family drop");
      break;
    }
    if (IsShuttingDown()) {
      s = Status::ShutdownInProgress();
      break;
    }
    s = db_impl->WriteWithCallback(wo, &write_batch.first, &write_batch.second);
    if (s.ok()) {
      // Key is successfully written to LSM
    } else if (s.IsBusy()) {
      // The key is overwritten in the meanwhile. Drop the blob record.
    } else {
      // We hit an error.
      break;
    }
  }
  if (s.IsBusy()) {
    s = Status::OK();
  }

  if (s.ok()) {
    db_impl->FlushWAL(true);
  }

  return s;
}

Status BlobGcJob::DeleteInputs() const {
  VersionEdit edit;
  edit.SetColumnFamilyID(blob_gc_->column_family_handle()->GetID());
  for (const auto& file : blob_gc_->sampled_inputs()) {
    ROCKS_LOG_BUFFER(log_buffer_, "Titan add obsolete file [%llu]",
                     file->file_number());
    edit.DeleteBlobFile(file->file_number());
  }
  auto s = version_set_->LogAndApply(&edit, this->mutex_);
  return s;
}

bool BlobGcJob::IsShuttingDown() {
  return (shutting_down_ && shutting_down_->load(std::memory_order_acquire));
}

void BlobGcJob::LogGcOnStart() {
  // Let's check if anything will get logged. Don't prepare all the info if
  // we're not logging
  if (db_options_.info_log_level <= InfoLogLevel::INFO_LEVEL) {
    char scratch[4096];
    blob_gc_->InputSummary(scratch, sizeof(scratch));
    ROCKS_LOG_INFO(db_options_.info_log, "[%s] GC start summary: %s\n",
                   blob_gc_->GetColumnFamilyData()->GetName().c_str(), scratch);
  }
}

void BlobGcJob::LogGcOnFinish() {
  // Let's check if anything will get logged. Don't prepare all the info if
  // we're not logging
  if (db_options_.info_log_level <= InfoLogLevel::INFO_LEVEL) {
    char scratch[4096];
    blob_gc_->OutputSummary(scratch, sizeof(scratch));
    ROCKS_LOG_INFO(db_options_.info_log, "[%s] GC finish summary: %s\n",
                   blob_gc_->GetColumnFamilyData()->GetName().c_str(), scratch);
  }
}

}  // namespace titandb
}  // namespace rocksdb
