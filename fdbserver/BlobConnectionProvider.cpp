/*
 * BlobConnectionProvider.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>

#include "flow/IRandom.h"
#include "fdbserver/BlobConnectionProvider.h"

struct SingleBlobConnectionProvider : BlobConnectionProvider {
public:
	std::pair<Reference<BackupContainerFileSystem>, std::string> createForWrite(std::string newFileName) {
		return std::pair(conn, newFileName);
	}

	Reference<BackupContainerFileSystem> getForRead(std::string filePath) { return conn; }

	SingleBlobConnectionProvider(std::string url) { conn = BackupContainerFileSystem::openContainerFS(url, {}, {}); }

private:
	Reference<BackupContainerFileSystem> conn;
};

struct PartitionedBlobConnectionProvider : BlobConnectionProvider {
	std::pair<Reference<BackupContainerFileSystem>, std::string> createForWrite(std::string newFileName) {
		// choose a partition randomly, to distribute load
		int writePartition = deterministicRandom()->randomInt(0, metadata.partitions.size());
		return std::pair(conn, metadata.partitions[writePartition].toString() + newFileName);
	}

	Reference<BackupContainerFileSystem> getForRead(std::string filePath) { return conn; }

	PartitionedBlobConnectionProvider(const Standalone<BlobMetadataDetailsRef> metadata) : metadata(metadata) {
		ASSERT(metadata.base.present());
		ASSERT(metadata.partitions.size() >= 2);
		conn = BackupContainerFileSystem::openContainerFS(metadata.base.get().toString(), {}, {});
		for (auto& it : metadata.partitions) {
			// these should be suffixes, not whole blob urls
			ASSERT(it.toString().find("://") == std::string::npos);
		}
	}

private:
	Standalone<BlobMetadataDetailsRef> metadata;
	Reference<BackupContainerFileSystem> conn;
};

// Could always include number of partitions as validation in sanity check or something?
// Ex: partition_numPartitions/filename instead of partition/filename
struct StorageLocationBlobConnectionProvider : BlobConnectionProvider {
	std::pair<Reference<BackupContainerFileSystem>, std::string> createForWrite(std::string newFileName) {
		// choose a partition randomly, to distribute load
		int writePartition = deterministicRandom()->randomInt(0, partitions.size());
		// include partition information in the filename
		return std::pair(partitions[writePartition], std::to_string(writePartition) + "/" + newFileName);
	}

	Reference<BackupContainerFileSystem> getForRead(std::string filePath) {
		size_t slash = filePath.find("/");
		ASSERT(slash != std::string::npos);
		int partition = stoi(filePath.substr(0, slash));
		ASSERT(partition >= 0);
		ASSERT(partition < partitions.size());
		return partitions[partition];
	}

	StorageLocationBlobConnectionProvider(const Standalone<BlobMetadataDetailsRef> metadata) {
		ASSERT(!metadata.base.present());
		ASSERT(metadata.partitions.size() >= 2);
		for (auto& it : metadata.partitions) {
			// these should be whole blob urls
			ASSERT(it.toString().find("://") != std::string::npos);
			partitions.push_back(BackupContainerFileSystem::openContainerFS(it.toString(), {}, {}));
		}
	}

private:
	std::vector<Reference<BackupContainerFileSystem>> partitions;
};

Reference<BlobConnectionProvider> BlobConnectionProvider::newBlobConnectionProvider(std::string blobUrl) {
	return makeReference<SingleBlobConnectionProvider>(blobUrl);
}

Reference<BlobConnectionProvider> BlobConnectionProvider::newBlobConnectionProvider(
    Standalone<BlobMetadataDetailsRef> blobMetadata) {
	if (blobMetadata.partitions.empty()) {
		return makeReference<SingleBlobConnectionProvider>(blobMetadata.base.get().toString());
	} else {
		ASSERT(blobMetadata.partitions.size() >= 2);
		if (blobMetadata.base.present()) {
			return makeReference<PartitionedBlobConnectionProvider>(blobMetadata);
		} else {
			return makeReference<StorageLocationBlobConnectionProvider>(blobMetadata);
		}
	}
}