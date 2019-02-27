#include "blob_gc_job_stats.h"

namespace rocksdb {
namespace titandb {

void BlobGcJobStats::Reset() {
  micros = 0;

  num_input_records = 0;
  num_input_files = 0;

  num_output_records = 0;
  num_output_files = 0;

  total_input_bytes = 0;
  total_output_bytes = 0;
  estimated_total_output_bytes = 0;

  num_records_replaced = 0;

  total_input_raw_key_bytes = 0;
  total_input_raw_value_bytes = 0;

  num_input_deletion_records = 0;
  num_expired_deletion_records = 0;

  num_corrupt_keys = 0;

  file_write_nanos = 0;
  file_range_sync_nanos = 0;
  file_fsync_nanos = 0;
  file_prepare_write_nanos = 0;
}

void BlobGcJobStats::Add(const BlobGcJobStats& stats) {
  micros += stats.micros;

  num_input_records += stats.num_input_records;
  num_input_files += stats.num_input_files;

  num_output_records += stats.num_output_records;
  num_output_files += stats.num_output_files;

  total_input_bytes += stats.total_input_bytes;
  total_output_bytes += stats.total_output_bytes;

  num_records_replaced += stats.num_records_replaced;

  total_input_raw_key_bytes += stats.total_input_raw_key_bytes;
  total_input_raw_value_bytes += stats.total_input_raw_value_bytes;
  estimated_total_output_bytes += stats.estimated_total_output_bytes;

  num_input_deletion_records += stats.num_input_deletion_records;
  num_expired_deletion_records += stats.num_expired_deletion_records;

  num_corrupt_keys += stats.num_corrupt_keys;

  file_write_nanos += stats.file_write_nanos;
  file_range_sync_nanos += stats.file_range_sync_nanos;
  file_fsync_nanos += stats.file_fsync_nanos;
  file_prepare_write_nanos += stats.file_prepare_write_nanos;
}

}  // namespace titandb
}  // namespace rocksdb
