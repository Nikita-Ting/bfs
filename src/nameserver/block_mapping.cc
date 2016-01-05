// Copyright (c) 2014, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "block_mapping.h"

#include <boost/bind.hpp>
#include <gflags/gflags.h>

#include "common/logging.h"

DECLARE_int32(default_replica_num);

namespace baidu {
namespace bfs {

BlockMapping::BlockMapping() :_next_block_id(1) {}

int64_t BlockMapping::NewBlockID() {
    MutexLock lock(&_mu, "BlockMapping::NewBlockID", 1000);
    return _next_block_id++;
}

bool BlockMapping::GetBlock(int64_t block_id, NSBlock* block) {
    MutexLock lock(&_mu, "BlockMapping::GetBlock", 1000);
    NSBlockMap::iterator it = _block_map.find(block_id);
    if (it == _block_map.end()) {
        return false;
    }
    if (block) {
        *block = *(it->second);
    }
    return true;
}

bool BlockMapping::MarkBlockStable(int64_t block_id) {
    MutexLock lock(&_mu);
    NSBlock* nsblock = NULL;
    NSBlockMap::iterator it = _block_map.find(block_id);
    if (it != _block_map.end()) {
        nsblock = it->second;
        //assert(nsblock->pending_change == true);
        nsblock->pending_change = false;
        return true;
    } else {
        LOG(WARNING, "Can't find block: #%ld ", block_id);
        return false;
    }
}

bool BlockMapping::GetReplicaLocation(int64_t id, std::set<int32_t>* chunkserver_id) {
    MutexLock lock(&_mu);
    NSBlock* nsblock = NULL;
    NSBlockMap::iterator it = _block_map.find(id);
    bool ret = false;
    if (it != _block_map.end()) {
        nsblock = it->second;
        *chunkserver_id = nsblock->replica;
        ret = true;
    } else {
        LOG(WARNING, "Can't find block: #%ld ", id);
    }

    return ret;
}

void BlockMapping::DealDeadBlocks(int32_t id, std::set<int64_t> blocks) {
    LOG(INFO, "Replicate %d blocks of dead chunkserver: %d\n", blocks.size(), id);
    MutexLock lock(&_mu);
    std::set<int64_t>::iterator it = blocks.begin();
    for (; it != blocks.end(); ++it) {
        //may have been unlinked, not in _block_map
        NSBlockMap::iterator nsb_it = _block_map.find(*it);
        if (nsb_it != _block_map.end()) {
            NSBlock* nsblock = nsb_it->second;
            nsblock->replica.erase(id);
            nsblock->pulling_chunkservers.erase(id);
            if (nsblock->pulling_chunkservers.empty() &&
                    nsblock->pending_change) {
                nsblock->pending_change = false;
            }
        }
    }
    _blocks_to_replicate.erase(id);
}

bool BlockMapping::ChangeReplicaNum(int64_t block_id, int32_t replica_num) {
    MutexLock lock(&_mu);
    NSBlockMap::iterator it = _block_map.find(block_id);
    if (it == _block_map.end()) {
        assert(0);
    } else {
        NSBlock* nsblock = it->second;
        nsblock->expect_replica_num = replica_num;
        return true;
    }
}

void BlockMapping::AddNewBlock(int64_t block_id) {
    MutexLock lock(&_mu);
    NSBlock* nsblock = NULL;
    NSBlockMap::iterator it = _block_map.find(block_id);
    //Don't suppport soft link now
    assert(it == _block_map.end());
    nsblock = new NSBlock(block_id);
    _block_map[block_id] = nsblock;
    LOG(DEBUG, "Init block info: #%ld ", block_id);
    if (_next_block_id <= block_id) {
        _next_block_id = block_id + 1;
    }
}

bool BlockMapping::UpdateBlockInfo(int64_t id, int32_t server_id, int64_t block_size,
                     int64_t block_version, int32_t* more_replica_num) {
    MutexLock lock(&_mu);
    NSBlock* nsblock = NULL;
    NSBlockMap::iterator it = _block_map.find(id);
    if (it == _block_map.end()) {
        //have been removed
        LOG(DEBUG, "UpdateBlockInfo(%ld) has been removed", id);
        return false;
    } else {
        nsblock = it->second;
        if (nsblock->version >= 0 && block_version >= 0 &&
                nsblock->version != block_version) {
            LOG(INFO, "block #%ld on slow chunkserver: %d,"
                    " NSB version: %ld, cs version: %ld, drop it",
                    id, server_id, nsblock->version, block_version);
            return false;
        }
        if (nsblock->block_size !=  block_size && block_size) {
            // update
            if (nsblock->block_size) {
                LOG(WARNING, "block #%ld size mismatch", id);
                assert(0);
                return false;
            } else {
                LOG(INFO, "block #%ld size update, %ld to %ld",
                    id, nsblock->block_size, block_size);
                nsblock->block_size = block_size;
            }
        } else {
            //LOG(DEBUG, "UpdateBlockInfo(%ld) ignored, from %ld to %ld",
            //    id, nsblock->block_size, block_size);
        }
    }
    std::pair<std::set<int32_t>::iterator, bool> ret = nsblock->replica.insert(server_id);
    int32_t cur_replica_num = nsblock->replica.size();
    int32_t expect_replica_num = nsblock->expect_replica_num;
    if (cur_replica_num != expect_replica_num) {
        if (!nsblock->pending_change) {
            nsblock->pending_change = true;
            if (cur_replica_num > expect_replica_num) {
                LOG(INFO, "too much replica cur=%d expect=%d server=%d",
                    server_id, cur_replica_num, expect_replica_num);
                nsblock->replica.erase(ret.first);
                return false;
            } else {
                // add new replica
                if (more_replica_num) {
                    *more_replica_num = expect_replica_num - cur_replica_num;
                    LOG(INFO, "Need to add %d new replica for #%ld cur=%d expect=%d",
                        *more_replica_num, id, cur_replica_num, expect_replica_num);
                }
            }
        }
    }
    return true;
}

void BlockMapping::RemoveBlocksForFile(const FileInfo& file_info) {
    for (int i = 0; i < file_info.blocks_size(); i++) {
        int64_t block_id = file_info.blocks(i);
        std::set<int32_t> chunkservers;
        GetReplicaLocation(block_id, &chunkservers);
        RemoveBlock(block_id);
        LOG(INFO, "Remove block #%ld for %s", block_id, file_info.name().c_str());
    }
}

void BlockMapping::RemoveBlock(int64_t block_id) {
    MutexLock lock(&_mu);
    NSBlockMap::iterator it = _block_map.find(block_id);
    if (it == _block_map.end()) {
        LOG(WARNING, "RemoveBlock(%ld) not found", block_id);
        return;
    }
    delete it->second;
    _block_map.erase(it);
}

bool BlockMapping::MarkPullBlock(int32_t dst_cs, int64_t block_id) {
    MutexLock lock(&_mu);
    NSBlockMap::iterator it = _block_map.find(block_id);
    assert(it != _block_map.end());
    bool ret = false;
    NSBlock* nsblock = it->second;
    if (nsblock->pulling_chunkservers.find(dst_cs) ==
            nsblock->pulling_chunkservers.end()) {
        nsblock->pulling_chunkservers.insert(dst_cs);
        _blocks_to_replicate[dst_cs].insert(block_id);
        LOG(INFO, "Add replicate info dst cs: %d, block #%ld",
                dst_cs, block_id);
        ret = true;
    }
    return ret;
}

void BlockMapping::UnmarkPullBlock(int32_t cs_id, int64_t block_id) {
    MutexLock lock(&_mu);
    NSBlockMap::iterator it = _block_map.find(block_id);
    if (it != _block_map.end()) {
        NSBlock* nsblock = it->second;
        assert(nsblock);
        nsblock->pulling_chunkservers.erase(cs_id);
        if (nsblock->pulling_chunkservers.empty() && nsblock->pending_change) {
            nsblock->pending_change = false;
            LOG(INFO, "Block #%ld on cs %d finish replicate\n", block_id, cs_id);
        }
        nsblock->replica.insert(cs_id);
    } else {
        LOG(WARNING, "Can't find block: #%ld ", block_id);
    }
}

bool BlockMapping::GetPullBlocks(int32_t id, std::vector<std::pair<int64_t, std::set<int32_t> > >* blocks) {
    MutexLock lock(&_mu);
    bool ret = false;
    std::map<int32_t, std::set<int64_t> >::iterator it = _blocks_to_replicate.find(id);
    if (it != _blocks_to_replicate.end()) {
        std::set<int64_t>::iterator block_it = it->second.begin();
        for (; block_it != it->second.end(); ++block_it) {
            blocks->push_back(std::make_pair(*block_it, _block_map[*block_it]->replica));
        }
        _blocks_to_replicate.erase(it);
        ret = true;
    }
    return ret;
}

bool BlockMapping::SetBlockVersion(int64_t block_id, int64_t version) {
    bool ret = true;
    MutexLock lock(&_mu);
    NSBlockMap::iterator it = _block_map.find(block_id);
    if (it == _block_map.end()) {
        LOG(WARNING, "Can't find block: #%ld ", block_id);
        ret = false;
    } else {
        it->second->version = version;
    }
    return ret;
}

} // namespace bfs
} // namespace baidu
