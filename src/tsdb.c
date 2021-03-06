/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "tsdb.h"

#include "config.h"
#include "consts.h"
#include "endianconv.h"
#include "indexer.h"
#include "module.h"

#include <assert.h>
#include <math.h>
#include "rmutil/alloc.h"
#include "rmutil/logging.h"
#include "rmutil/strings.h"

static Series *lastDeletedSeries = NULL;
static RedisModuleString *renameFromKey = NULL;

int dictOperator(RedisModuleDict *d, void *chunk, timestamp_t ts, DictOp op) {
    timestamp_t rax_key = htonu64(ts);
    switch (op) {
        case DICT_OP_SET:
            return RedisModule_DictSetC(d, &rax_key, sizeof(rax_key), chunk);
        case DICT_OP_REPLACE:
            return RedisModule_DictReplaceC(d, &rax_key, sizeof(rax_key), chunk);
        case DICT_OP_DEL:
            return RedisModule_DictDelC(d, &rax_key, sizeof(rax_key), NULL);
    }
    chunk = NULL;
    return REDISMODULE_OK; // silence compiler
}

Series *NewSeries(RedisModuleString *keyName, CreateCtx *cCtx) {
    Series *newSeries = (Series *)malloc(sizeof(Series));
    newSeries->keyName = keyName;
    newSeries->chunks = RedisModule_CreateDict(NULL);
    newSeries->chunkSizeBytes = cCtx->chunkSizeBytes;
    newSeries->retentionTime = cCtx->retentionTime;
    newSeries->srcKey = NULL;
    newSeries->rules = NULL;
    newSeries->lastTimestamp = 0;
    newSeries->lastValue = 0;
    newSeries->totalSamples = 0;
    newSeries->labels = cCtx->labels;
    newSeries->labelsCount = cCtx->labelsCount;
    newSeries->options = cCtx->options;
    newSeries->duplicatePolicy = cCtx->duplicatePolicy;

    if (newSeries->options & SERIES_OPT_UNCOMPRESSED) {
        newSeries->options |= SERIES_OPT_UNCOMPRESSED;
        newSeries->funcs = GetChunkClass(CHUNK_REGULAR);
    } else {
        newSeries->funcs = GetChunkClass(CHUNK_COMPRESSED);
    }
    Chunk_t *newChunk = newSeries->funcs->NewChunk(newSeries->chunkSizeBytes);
    dictOperator(newSeries->chunks, newChunk, 0, DICT_OP_SET);
    newSeries->lastChunk = newChunk;
    return newSeries;
}

void SeriesTrim(Series *series) {
    if (series->retentionTime == 0) {
        return;
    }

    // start iterator from smallest key
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, "^", NULL, 0);
    Chunk_t *currentChunk;
    void *currentKey;
    size_t keyLen;
    timestamp_t minTimestamp = series->lastTimestamp > series->retentionTime
                                   ? series->lastTimestamp - series->retentionTime
                                   : 0;

    while ((currentKey = RedisModule_DictNextC(iter, &keyLen, (void *)&currentChunk))) {
        if (series->funcs->GetLastTimestamp(currentChunk) < minTimestamp) {
            RedisModule_DictDelC(series->chunks, currentKey, keyLen, NULL);
            // reseek iterator since we modified the dict,
            // go to first element that is bigger than current key
            RedisModule_DictIteratorReseekC(iter, ">", currentKey, keyLen);

            series->totalSamples -= series->funcs->GetNumOfSample(currentChunk);
            series->funcs->FreeChunk(currentChunk);
        } else {
            break;
        }
    }
    RedisModule_DictIteratorStop(iter);
}

// Encode timestamps as bigendian to allow correct lexical sorting
static void seriesEncodeTimestamp(void *buf, timestamp_t timestamp) {
    uint64_t e;
    e = htonu64(timestamp);
    memcpy(buf, &e, sizeof(e));
}

void freeLastDeletedSeries() {
    if (lastDeletedSeries == NULL) {
        return;
    }
    CompactionRule *rule = lastDeletedSeries->rules;
    while (rule != NULL) {
        CompactionRule *nextRule = rule->nextRule;
        FreeCompactionRule(rule);
        rule = nextRule;
    }
    if (lastDeletedSeries->srcKey != NULL) {
        RedisModule_FreeString(NULL, lastDeletedSeries->srcKey);
    }
    RedisModule_FreeString(NULL, lastDeletedSeries->keyName);
    free(lastDeletedSeries);
    lastDeletedSeries = NULL;
}

void CleanLastDeletedSeries(RedisModuleString *key) {
    if (lastDeletedSeries != NULL &&
        RedisModule_StringCompare(lastDeletedSeries->keyName, key) == 0) {
        RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
        RedisModule_AutoMemory(ctx);

        CompactionRule *rule = lastDeletedSeries->rules;
        while (rule != NULL) {
            RedisModuleKey *seriesKey;
            Series *dstSeries;
            const int status = GetSeries(
                ctx, rule->destKey, &seriesKey, &dstSeries, REDISMODULE_READ | REDISMODULE_WRITE);
            if (status) {
                SeriesDeleteSrcRule(dstSeries, lastDeletedSeries->keyName);
                RedisModule_CloseKey(seriesKey);
            }
            rule = rule->nextRule;
        }
        if (lastDeletedSeries->srcKey) {
            RedisModuleKey *seriesKey;
            Series *srcSeries;
            const int status = GetSeries(ctx,
                                         lastDeletedSeries->srcKey,
                                         &seriesKey,
                                         &srcSeries,
                                         REDISMODULE_READ | REDISMODULE_WRITE);
            if (status) {
                SeriesDeleteRule(srcSeries, lastDeletedSeries->keyName);
                RedisModule_CloseKey(seriesKey);
            }
        }

        RedisModule_FreeThreadSafeContext(ctx);
    }
    freeLastDeletedSeries();
}

void RenameSeriesFrom(RedisModuleCtx *ctx, RedisModuleString *key) {
    // keep in global variable for RenameSeriesTo() and increase recount
    RedisModule_RetainString(NULL, key);
    renameFromKey = key;
}

void RenameSeriesTo(RedisModuleCtx *ctx, RedisModuleString *keyTo) {
    // Try to open the series
    Series *series;
    RedisModuleKey *key = NULL;
    const int status = SilentGetSeries(ctx, keyTo, &key, &series, REDISMODULE_READ);
    if (!status) { // Not a timeseries key
        goto cleanup;
    }

    // Reindex key by the new name
    RemoveIndexedMetric(ctx, renameFromKey, series->labels, series->labelsCount);
    IndexMetric(ctx, keyTo, series->labels, series->labelsCount);

    // A destination key was renamed
    if (series->srcKey) {
        Series *srcSeries;
        RedisModuleKey *srcKey;
        const int status =
            SilentGetSeries(ctx, series->srcKey, &srcKey, &srcSeries, REDISMODULE_WRITE);
        if (!status) {
            const char *srcKeyName = RedisModule_StringPtrLen(series->srcKey, NULL);
            RedisModule_Log(
                ctx, "warning", "couldn't open key or key is not a Timeseries. key=%s", srcKeyName);
            goto cleanup;
        }

        // Find the rule in the source key and rename the its destKey
        CompactionRule *rule = srcSeries->rules;
        while (rule) {
            if (RedisModule_StringCompare(renameFromKey, rule->destKey) == 0) {
                RedisModule_FreeString(NULL, rule->destKey);
                RedisModule_RetainString(NULL, keyTo);
                rule->destKey = keyTo;
                break; // Only one src can point back to destKey
            }
            rule = rule->nextRule;
        }
        RedisModule_CloseKey(srcKey);
    }

    // A source key was renamed need to rename the srcKey on all the destKeys
    if (series->rules) {
        CompactionRule *rule = series->rules;
        Series *destSeries;
        RedisModuleKey *destKey;
        while (rule) {
            const int status =
                SilentGetSeries(ctx, rule->destKey, &destKey, &destSeries, REDISMODULE_WRITE);
            if (!status) {
                const char *destKeyName = RedisModule_StringPtrLen(rule->destKey, NULL);
                RedisModule_Log(ctx,
                                "warning",
                                "couldn't open key or key is not a Timeseries. key=%s",
                                destKeyName);
            } else {
                // rename the srcKey in the destKey
                RedisModule_FreeString(NULL, destSeries->srcKey);
                RedisModule_RetainString(NULL, keyTo);
                destSeries->srcKey = keyTo;

                RedisModule_CloseKey(destKey);
            }
            rule = rule->nextRule;
        }
    }

cleanup:
    if (key) {
        RedisModule_CloseKey(key);
    }
    RedisModule_FreeString(NULL, renameFromKey);
    renameFromKey = NULL;
}

// Releases Series and all its compaction rules
void FreeSeries(void *value) {
    Series *currentSeries = (Series *)value;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(currentSeries->chunks, "^", NULL, 0);
    Chunk_t *currentChunk;
    while (RedisModule_DictNextC(iter, NULL, (void *)&currentChunk) != NULL) {
        currentSeries->funcs->FreeChunk(currentChunk);
    }
    RedisModule_DictIteratorStop(iter);

    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
    RedisModule_AutoMemory(ctx);
    RemoveIndexedMetric(
        ctx, currentSeries->keyName, currentSeries->labels, currentSeries->labelsCount);

    FreeLabels(currentSeries->labels, currentSeries->labelsCount);

    RedisModule_FreeThreadSafeContext(ctx);
    RedisModule_FreeDict(NULL, currentSeries->chunks);

    freeLastDeletedSeries();
    lastDeletedSeries = currentSeries;
}

void FreeCompactionRule(void *value) {
    CompactionRule *rule = (CompactionRule *)value;
    RedisModule_FreeString(NULL, rule->destKey);
    ((AggregationClass *)rule->aggClass)->freeContext(rule->aggContext);
    free(rule);
}

size_t SeriesGetChunksSize(Series *series) {
    size_t size = 0;
    Chunk_t *currentChunk;
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(series->chunks, "^", NULL, 0);
    while (RedisModule_DictNextC(iter, NULL, (void *)&currentChunk)) {
        size += series->funcs->GetChunkSize(currentChunk, true);
    }
    RedisModule_DictIteratorStop(iter);
    return size;
}

size_t SeriesMemUsage(const void *value) {
    Series *series = (Series *)value;

    size_t labelLen = 0;
    uint32_t labelsLen = 0;
    for (int i = 0; i < series->labelsCount; i++) {
        RedisModule_StringPtrLen(series->labels[i].key, &labelLen);
        labelsLen += (labelLen + 1);
        RedisModule_StringPtrLen(series->labels[i].value, &labelLen);
        labelsLen += (labelLen + 1);
    }

    size_t rulesSize = 0;
    CompactionRule *rule = series->rules;
    while (rule != NULL) {
        rulesSize += sizeof(CompactionRule);
        rule = rule->nextRule;
    }

    return sizeof(series) + rulesSize + labelsLen + sizeof(Label) * series->labelsCount +
           SeriesGetChunksSize(series);
}

size_t SeriesGetNumSamples(const Series *series) {
    size_t numSamples = 0;
    if (series != NULL) {
        numSamples = series->totalSamples;
    }
    return numSamples;
}

static void upsertCompaction(Series *series, UpsertCtx *uCtx) {
    CompactionRule *rule = series->rules;
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
    const timestamp_t upsertTimestamp = uCtx->sample.timestamp;
    const timestamp_t seriesLastTimestamp = series->lastTimestamp;
    while (rule != NULL) {
        const timestamp_t ruleTimebucket = rule->timeBucket;
        const timestamp_t curAggWindowStart = CalcWindowStart(seriesLastTimestamp, ruleTimebucket);
        if (upsertTimestamp >= curAggWindowStart) {
            // upsert in latest timebucket
            const int rv = SeriesCalcRange(series, curAggWindowStart, UINT64_MAX, rule, NULL);
            if (rv == TSDB_ERROR) {
                RedisModule_Log(ctx, "verbose", "%s", "Failed to calculate range for downsample");
                continue;
            }
        } else {
            const timestamp_t start = CalcWindowStart(upsertTimestamp, ruleTimebucket);
            // ensure last include/exclude
            double val = 0;
            const int rv = SeriesCalcRange(series, start, start + ruleTimebucket - 1, rule, &val);
            if (rv == TSDB_ERROR) {
                RedisModule_Log(ctx, "verbose", "%s", "Failed to calculate range for downsample");
                continue;
            }

            RedisModuleKey *key;
            Series *destSeries;
            if (!GetSeries(ctx, rule->destKey, &key, &destSeries, REDISMODULE_READ)) {
                RedisModule_Log(ctx, "verbose", "%s", "Failed to retrieve downsample series");
                continue;
            }
            if (destSeries->totalSamples == 0) {
                SeriesAddSample(destSeries, start, val);
            } else {
                SeriesUpsertSample(destSeries, start, val, DP_LAST);
            }
            RedisModule_CloseKey(key);
        }
        rule = rule->nextRule;
    }
    RedisModule_FreeThreadSafeContext(ctx);
}

int SeriesUpsertSample(Series *series,
                       api_timestamp_t timestamp,
                       double value,
                       DuplicatePolicy dp_override) {
    bool latestChunk = true;
    void *chunkKey = NULL;
    ChunkFuncs *funcs = series->funcs;
    Chunk_t *chunk = series->lastChunk;
    timestamp_t chunkFirstTS = funcs->GetFirstTimestamp(series->lastChunk);

    if (timestamp < chunkFirstTS && RedisModule_DictSize(series->chunks) > 1) {
        // Upsert in an older chunk
        latestChunk = false;
        timestamp_t rax_key;
        seriesEncodeTimestamp(&rax_key, timestamp);
        RedisModuleDictIter *dictIter =
            RedisModule_DictIteratorStartC(series->chunks, "<=", &rax_key, sizeof(rax_key));
        chunkKey = RedisModule_DictNextC(dictIter, NULL, (void *)&chunk);
        if (chunkKey == NULL) {
            RedisModule_DictIteratorReseekC(dictIter, "^", NULL, 0);
            chunkKey = RedisModule_DictNextC(dictIter, NULL, (void *)&chunk);
        }
        RedisModule_DictIteratorStop(dictIter);
        if (chunkKey == NULL) {
            return REDISMODULE_ERR;
        }
        chunkFirstTS = funcs->GetFirstTimestamp(chunk);
    }

    // Split chunks
    if (funcs->GetChunkSize(chunk, false) > series->chunkSizeBytes * SPLIT_FACTOR) {
        Chunk_t *newChunk = funcs->SplitChunk(chunk);
        if (newChunk == NULL) {
            return REDISMODULE_ERR;
        }
        timestamp_t newChunkFirstTS = funcs->GetFirstTimestamp(newChunk);
        dictOperator(series->chunks, newChunk, newChunkFirstTS, DICT_OP_SET);
        if (timestamp >= newChunkFirstTS) {
            chunk = newChunk;
            chunkFirstTS = newChunkFirstTS;
        }
        if (latestChunk) { // split of latest chunk
            series->lastChunk = newChunk;
        }
    }

    UpsertCtx uCtx = {
        .inChunk = chunk,
        .sample = { .timestamp = timestamp, .value = value },
    };

    int size = 0;

    // Use module level configuration if key level configuration doesn't exists
    DuplicatePolicy dp_policy;
    if (dp_override != DP_NONE) {
        dp_policy = dp_override;
    } else if (series->duplicatePolicy != DP_NONE) {
        dp_policy = series->duplicatePolicy;
    } else {
        dp_policy = TSGlobalConfig.duplicatePolicy;
    }

    ChunkResult rv = funcs->UpsertSample(&uCtx, &size, dp_policy);
    if (rv == CR_OK) {
        series->totalSamples += size;
        if (timestamp == series->lastTimestamp) {
            series->lastValue = uCtx.sample.value;
        }
        timestamp_t chunkFirstTSAfterOp = funcs->GetFirstTimestamp(uCtx.inChunk);
        if (chunkFirstTSAfterOp != chunkFirstTS) {
            // update chunk in dictionary if first timestamp changed
            if (dictOperator(series->chunks, NULL, chunkFirstTS, DICT_OP_DEL) == REDISMODULE_ERR) {
                dictOperator(series->chunks, NULL, 0, DICT_OP_DEL);
            }
            dictOperator(series->chunks, uCtx.inChunk, chunkFirstTSAfterOp, DICT_OP_SET);
        }

        upsertCompaction(series, &uCtx);
    }
    return rv;
}

int SeriesAddSample(Series *series, api_timestamp_t timestamp, double value) {
    // backfilling or update
    Sample sample = { .timestamp = timestamp, .value = value };
    ChunkResult ret = series->funcs->AddSample(series->lastChunk, &sample);

    if (ret == CR_END) {
        // When a new chunk is created trim the series
        SeriesTrim(series);

        Chunk_t *newChunk = series->funcs->NewChunk(series->chunkSizeBytes);
        dictOperator(series->chunks, newChunk, timestamp, DICT_OP_SET);
        ret = series->funcs->AddSample(newChunk, &sample);
        series->lastChunk = newChunk;
    }
    series->lastTimestamp = timestamp;
    series->lastValue = value;
    series->totalSamples++;
    return TSDB_OK;
}

static int SeriesChunkIteratorOptions(SeriesIterator *iter) {
    int options = 0;
    if (iter->reverse) {
        options |= CHUNK_ITER_OP_REVERSE;
    }
    return options;
}

// Initiates SeriesIterator, find the correct chunk and initiate a ChunkIterator
SeriesIterator SeriesQuery(Series *series, timestamp_t start_ts, timestamp_t end_ts, bool rev) {
    SeriesIterator iter = { 0 };
    iter.series = series;
    iter.minTimestamp = start_ts;
    iter.maxTimestamp = end_ts;
    iter.reverse = rev;

    timestamp_t rax_key;
    ChunkFuncs *funcs = series->funcs;

    if (iter.reverse == false) {
        iter.DictGetNext = RedisModule_DictNextC;
        seriesEncodeTimestamp(&rax_key, iter.minTimestamp);
    } else {
        iter.DictGetNext = RedisModule_DictPrevC;
        seriesEncodeTimestamp(&rax_key, iter.maxTimestamp);
    }

    // get first chunk within query range
    iter.dictIter = RedisModule_DictIteratorStartC(series->chunks, "<=", &rax_key, sizeof(rax_key));
    if (!iter.DictGetNext(iter.dictIter, NULL, (void *)&iter.currentChunk)) {
        RedisModule_DictIteratorReseekC(iter.dictIter, "^", NULL, 0);
        iter.DictGetNext(iter.dictIter, NULL, (void *)&iter.currentChunk);
    }

    iter.chunkIterator = funcs->NewChunkIterator(
        iter.currentChunk, SeriesChunkIteratorOptions(&iter), &iter.chunkIteratorFuncs);
    return iter;
}

// this is an internal function that routes the next call to the appropriate chunk iterator function
static ChunkResult SeriesGetNext(SeriesIterator *iter, Sample *sample) {
    if (iter->reverse == false) {
        return iter->chunkIteratorFuncs.GetNext(iter->chunkIterator, sample);
    } else {
        if (iter->chunkIteratorFuncs.GetPrev == NULL) {
            return CR_ERR;
        }
        return iter->chunkIteratorFuncs.GetPrev(iter->chunkIterator, sample);
    }
}

void SeriesIteratorClose(SeriesIterator *iterator) {
    iterator->chunkIteratorFuncs.Free(iterator->chunkIterator);
    RedisModule_DictIteratorStop(iterator->dictIter);
}

// Fills sample from chunk. If all samples were extracted from the chunk, we
// move to the next chunk.
ChunkResult SeriesIteratorGetNext(SeriesIterator *iterator, Sample *currentSample) {
    ChunkResult res;
    ChunkFuncs *funcs = iterator->series->funcs;
    Chunk_t *currentChunk = iterator->currentChunk;

    while (true) {
        res = SeriesGetNext(iterator, currentSample);
        if (res == CR_END) { // Reached the end of the chunk
            if (!iterator->DictGetNext(iterator->dictIter, NULL, (void *)&currentChunk) ||
                funcs->GetFirstTimestamp(currentChunk) > iterator->maxTimestamp ||
                funcs->GetLastTimestamp(currentChunk) < iterator->minTimestamp) {
                return CR_END; // No more chunks or they out of range
            }
            iterator->chunkIteratorFuncs.Free(iterator->chunkIterator);
            iterator->chunkIterator = funcs->NewChunkIterator(
                currentChunk, SeriesChunkIteratorOptions(iterator), &iterator->chunkIteratorFuncs);
            if (SeriesGetNext(iterator, currentSample) != CR_OK) {
                return CR_END;
            }
        } else if (res == CR_ERR) {
            return CR_ERR;
        }

        // check timestamp is within range
        if (!iterator->reverse) {
            // forward range handling
            if (currentSample->timestamp < iterator->minTimestamp) {
                // didn't reach the starting point of the requested range
                continue;
            }
            if (currentSample->timestamp > iterator->maxTimestamp) {
                // reached the end of the requested range
                return CR_END;
            }
        } else {
            // reverse range handling
            if (currentSample->timestamp > iterator->maxTimestamp) {
                // didn't reach our starting range
                continue;
            }
            if (currentSample->timestamp < iterator->minTimestamp) {
                // didn't reach the starting point of the requested range
                return CR_END;
            }
        }
        return CR_OK;
    }
    return CR_OK;
}

CompactionRule *SeriesAddRule(Series *series,
                              RedisModuleString *destKeyStr,
                              int aggType,
                              uint64_t timeBucket) {
    CompactionRule *rule = NewRule(destKeyStr, aggType, timeBucket);
    if (rule == NULL) {
        return NULL;
    }
    if (series->rules == NULL) {
        series->rules = rule;
    } else {
        CompactionRule *last = series->rules;
        while (last->nextRule != NULL)
            last = last->nextRule;
        last->nextRule = rule;
    }
    return rule;
}

int SeriesCreateRulesFromGlobalConfig(RedisModuleCtx *ctx,
                                      RedisModuleString *keyName,
                                      Series *series,
                                      Label *labels,
                                      size_t labelsCount) {
    size_t len;
    int i;
    Series *compactedSeries;
    RedisModuleKey *compactedKey;
    size_t compactedRuleLabelCount = labelsCount + 2;

    for (i = 0; i < TSGlobalConfig.compactionRulesCount; i++) {
        SimpleCompactionRule *rule = TSGlobalConfig.compactionRules + i;
        const char *aggString = AggTypeEnumToString(rule->aggType);
        RedisModuleString *destKey = RedisModule_CreateStringPrintf(
            ctx, "%s_%s_%ld", RedisModule_StringPtrLen(keyName, &len), aggString, rule->timeBucket);
        RedisModule_RetainString(ctx, destKey);
        compactedKey = RedisModule_OpenKey(ctx, destKey, REDISMODULE_READ | REDISMODULE_WRITE);
        if (RedisModule_KeyType(compactedKey) != REDISMODULE_KEYTYPE_EMPTY) {
            // TODO: should we break here? Is log enough?
            RM_LOG_WARNING(ctx, "Cannot create compacted key, key '%s' already exists", destKey);
            RedisModule_FreeString(ctx, destKey);
            RedisModule_CloseKey(compactedKey);
            continue;
        }
        SeriesAddRule(series, destKey, rule->aggType, rule->timeBucket);

        Label *compactedLabels = malloc(sizeof(Label) * compactedRuleLabelCount);
        // todo: deep copy labels function
        for (int l = 0; l < labelsCount; l++) {
            compactedLabels[l].key = RedisModule_CreateStringFromString(NULL, labels[l].key);
            compactedLabels[l].value = RedisModule_CreateStringFromString(NULL, labels[l].value);
        }

        // For every aggregated key create 2 labels: `aggregation` and `time_bucket`.
        compactedLabels[labelsCount].key = RedisModule_CreateStringPrintf(NULL, "aggregation");
        compactedLabels[labelsCount].value =
            RedisModule_CreateString(NULL, aggString, strlen(aggString));
        compactedLabels[labelsCount + 1].key = RedisModule_CreateStringPrintf(NULL, "time_bucket");
        compactedLabels[labelsCount + 1].value =
            RedisModule_CreateStringPrintf(NULL, "%ld", rule->timeBucket);

        CreateCtx cCtx = {
            .retentionTime = rule->retentionSizeMillisec,
            .chunkSizeBytes = TSGlobalConfig.chunkSizeBytes,
            .labelsCount = compactedRuleLabelCount,
            .labels = compactedLabels,
            .options = TSGlobalConfig.options & SERIES_OPT_UNCOMPRESSED,
        };
        CreateTsKey(ctx, destKey, &cCtx, &compactedSeries, &compactedKey);
        RedisModule_CloseKey(compactedKey);
    }
    return TSDB_OK;
}

CompactionRule *NewRule(RedisModuleString *destKey, int aggType, uint64_t timeBucket) {
    if (timeBucket == 0ULL) {
        return NULL;
    }

    CompactionRule *rule = (CompactionRule *)malloc(sizeof(CompactionRule));
    rule->aggClass = GetAggClass(aggType);
    ;
    rule->aggType = aggType;
    rule->aggContext = rule->aggClass->createContext();
    rule->timeBucket = timeBucket;
    rule->destKey = destKey;
    rule->startCurrentTimeBucket = -1LL;
    rule->nextRule = NULL;

    return rule;
}

int SeriesDeleteRule(Series *series, RedisModuleString *destKey) {
    CompactionRule *rule = series->rules;
    CompactionRule *prev_rule = NULL;
    while (rule != NULL) {
        if (RMUtil_StringEquals(rule->destKey, destKey)) {
            CompactionRule *next = rule->nextRule;
            FreeCompactionRule(rule);
            if (prev_rule != NULL) {
                // cut off the current rule from the linked list
                prev_rule->nextRule = next;
            } else {
                // make the next one to be the first rule
                series->rules = next;
            }
            return TRUE;
        }
        prev_rule = rule;
        rule = rule->nextRule;
    }
    return FALSE;
}

int SeriesSetSrcRule(Series *series, RedisModuleString *srctKey) {
    if (series->srcKey) {
        return FALSE;
    }
    series->srcKey = srctKey;
    return TRUE;
}

int SeriesDeleteSrcRule(Series *series, RedisModuleString *srctKey) {
    if (RMUtil_StringEquals(series->srcKey, srctKey)) {
        RedisModule_FreeString(NULL, series->srcKey);
        series->srcKey = NULL;
        return TRUE;
    }
    return FALSE;
}

/*
 * This function calculate aggregation value of a range.
 *
 * If `val` is NULL, the function will update the context of `rule`.
 */
int SeriesCalcRange(Series *series,
                    timestamp_t start_ts,
                    timestamp_t end_ts,
                    CompactionRule *rule,
                    double *val) {
    AggregationClass *aggObject = rule->aggClass;

    Sample sample = { 0 };
    SeriesIterator iterator = SeriesQuery(series, start_ts, end_ts, false);
    if (iterator.series == NULL) {
        return TSDB_ERROR;
    }
    void *context = aggObject->createContext();

    while (SeriesIteratorGetNext(&iterator, &sample) == CR_OK) {
        aggObject->appendValue(context, sample.value);
    }
    SeriesIteratorClose(&iterator);
    if (val == NULL) { // just update context for current window
        aggObject->freeContext(rule->aggContext);
        rule->aggContext = context;
    } else {
        aggObject->finalize(context, val);
        aggObject->freeContext(context);
    }
    return TSDB_OK;
}

timestamp_t CalcWindowStart(timestamp_t timestamp, size_t window) {
    return timestamp - (timestamp % window);
}

timestamp_t getFirstValidTimestamp(Series *series, long long *skipped) {
    *skipped = 0;
    if (series->totalSamples == 0) {
        return 0;
    }

    size_t i = 0;
    Sample sample = { 0 };

    timestamp_t minTimestamp = 0;
    if (series->retentionTime && series->retentionTime < series->lastTimestamp) {
        minTimestamp = series->lastTimestamp - series->retentionTime;
    }

    SeriesIterator iterator = SeriesQuery(series, 0, series->lastTimestamp, FALSE);
    ChunkResult result = SeriesIteratorGetNext(&iterator, &sample);

    while (result == CR_OK && sample.timestamp < minTimestamp) {
        result = SeriesIteratorGetNext(&iterator, &sample);
        ++i;
    }

    *skipped = i;
    SeriesIteratorClose(&iterator);
    return sample.timestamp;
}
