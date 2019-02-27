#pragma once

#include "util/file_reader_writer.h"
#include "utilities/titandb/blob_format.h"

namespace rocksdb {
namespace titandb {

// Contains information to complete a blob file creation.
class BlobFileHandle {
 public:
  virtual ~BlobFileHandle() {}

  virtual uint64_t GetNumber() const = 0;

  virtual const std::string& GetName() const = 0;

  virtual WritableFileWriter* GetFile() const = 0;
};

// Manages the process of blob files creation.
class BlobFileManager {
 public:
  struct BlobFilePair {
    std::shared_ptr<BlobFileMeta> meta;
    std::unique_ptr<BlobFileHandle> handle;
  };

  virtual ~BlobFileManager() {}

  // Creates a new file. The new file should not be accessed until
  // FinishFile() has been called.
  // If successful, sets "*handle* to the new file handle.
  virtual Status NewFile(std::unique_ptr<BlobFileHandle>* handle) = 0;

  // Finishes the file with the provided metadata. Stops writing to
  // the file anymore.
  // REQUIRES: FinishFile(), DeleteFile() have not been called.
  virtual Status FinishFile(uint32_t cf_id, std::shared_ptr<BlobFileMeta> file,
                            std::unique_ptr<BlobFileHandle>&& handle) {
    std::vector<BlobFilePair> tmp;
    tmp.emplace_back(BlobFilePair{file, std::move(handle)});
    return BatchFinishFiles(cf_id, tmp);
  }

  // Batch version of FinishFile
  virtual Status BatchFinishFiles(uint32_t cf_id,
                                  const std::vector<BlobFilePair>& files) {
    (void)cf_id;
    (void)files;
    return Status::OK();
  };

  // Deletes the file. If the caller is not going to call
  // FinishFile(), it must call DeleteFile() to release the handle.
  // REQUIRES: FinishFile(), DeleteFile() have not been called.
  virtual Status DeleteFile(std::unique_ptr<BlobFileHandle>&& handle) {
    std::vector<std::unique_ptr<BlobFileHandle>> tmp;
    tmp.emplace_back(std::move(handle));
    return BatchDeleteFiles(tmp);
  }

  // Batch version of DeleteFile
  virtual Status BatchDeleteFiles(
      const std::vector<std::unique_ptr<BlobFileHandle>>& handles) {
    (void)handles;
    return Status::OK();
  }
};

}  // namespace titandb
}  // namespace rocksdb
