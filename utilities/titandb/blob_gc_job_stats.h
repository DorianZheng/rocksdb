#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rocksdb {
namespace titandb {

struct BlobGcJobStats {
  BlobGcJobStats() { Reset(); }
  void Reset();
  // Aggregate the BlogGcJobStats from another instance with this one
  void Add(const BlobGcJobStats& stats);

  // the total elapsed time of this gc in microseconds.
  uint64_t micros;

  // the elapsed time of sampling of this gc in microseconds.
  uint64_t sampling_micros;

  // the number of gc input records.
  uint64_t num_input_records;
  // the number of gc input files.
  size_t num_input_files;

  // the number of gc output records.
  uint64_t num_output_records;
  // the number of gc output files.
  size_t num_output_files;

  // the size of the gc input in bytes.
  uint64_t total_input_bytes;
  // the size of the gc output in bytes.
  uint64_t total_output_bytes;
  // the estimated size of the gc output in bytes.
  uint64_t estimated_total_output_bytes = 0;

  // number of records being replaced by newer record associated with same key.
  // this could be a new value or a deletion entry for that key so this field
  // sums up all updated and deleted keys
  uint64_t num_records_replaced;

  // the sum of the uncompressed input keys in bytes.
  uint64_t total_input_raw_key_bytes;
  // the sum of the uncompressed input values in bytes.
  uint64_t total_input_raw_value_bytes;

  // the number of deletion entries before gc. Deletion entries
  // can disappear after gc because they expired
  uint64_t num_input_deletion_records;
  // number of deletion records that were found obsolete and discarded
  // because it is not possible to delete any more keys with this entry
  // (i.e. all possible deletions resulting from it have been completed)
  uint64_t num_expired_deletion_records;

  // number of corrupt keys (ParseInternalKey returned false when applied to
  // the key) encountered and written out.
  uint64_t num_corrupt_keys;

  // Following counters are only populated if
  // options.report_bg_io_stats = true;

  // Time spent on file's Append() call.
  uint64_t file_write_nanos;

  // Time spent on sync file range.
  uint64_t file_range_sync_nanos;

  // Time spent on file fsync.
  uint64_t file_fsync_nanos;

  // Time spent on preparing file write (fallocate, etc)
  uint64_t file_prepare_write_nanos;
};

}  // namespace titandb
}  // namespace rocksdb