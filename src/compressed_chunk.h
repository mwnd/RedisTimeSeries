/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#ifndef COMPRESSED_CHUNK_H
#define COMPRESSED_CHUNK_H

#include "generic_chunk.h"
#include "gorilla.h"

#include <stdbool.h>   // bool
#include <sys/types.h> // u_int_t

// Initialize compressed chunk
Chunk_t *Compressed_NewChunk(size_t size);
void Compressed_FreeChunk(Chunk_t *chunk);
Chunk_t *Compressed_SplitChunk(Chunk_t *chunk);

// Append a sample to a compressed chunk
ChunkResult Compressed_AddSample(Chunk_t *chunk, Sample *sample);
ChunkResult Compressed_UpsertSample(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy);

// Read from compressed chunk using an iterator
ChunkIter_t *Compressed_NewChunkIterator(Chunk_t *chunk,
                                         int options,
                                         ChunkIterFuncs *retChunkIterClass);
ChunkResult Compressed_ChunkIteratorGetNext(ChunkIter_t *iter, Sample *sample);
void Compressed_FreeChunkIterator(ChunkIter_t *iter);

// Miscellaneous
size_t Compressed_GetChunkSize(Chunk_t *chunk, bool includeStruct);
u_int64_t Compressed_ChunkNumOfSample(Chunk_t *chunk);
timestamp_t Compressed_GetFirstTimestamp(Chunk_t *chunk);
timestamp_t Compressed_GetLastTimestamp(Chunk_t *chunk);

// RDB
void Compressed_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io);
void Compressed_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io);

/* Used in tests */
u_int64_t getIterIdx(ChunkIter_t *iter);

#endif // COMPRESSED_CHUNK_H
