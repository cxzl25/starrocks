// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.lake;

import com.staros.proto.ObjectStorageInfo;
import com.staros.proto.ShardStorageInfo;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.DistributionInfo;
import com.starrocks.catalog.KeysType;
import com.starrocks.catalog.MaterializedIndex;
import com.starrocks.catalog.MaterializedView;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.Partition;
import com.starrocks.catalog.PartitionInfo;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.TableIndexes;
import com.starrocks.catalog.TableProperty;
import com.starrocks.catalog.Tablet;
import com.starrocks.catalog.TabletInvertedIndex;
import com.starrocks.common.DdlException;
import com.starrocks.common.io.DeepCopy;
import com.starrocks.common.io.Text;
import com.starrocks.persist.gson.GsonUtils;
import com.starrocks.server.GlobalStateMgr;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.net.URI;
import java.net.URISyntaxException;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Metadata for StarRocks lake table
 * <p>
 * Currently, storage group is table level, which stores all the tablets data and metadata of this table.
 * Format: service storage uri (from StarOS) + table id
 * <p>
 * TODO: support table api like Iceberg
 */
public class LakeTable extends OlapTable {

    private static final Logger LOG = LogManager.getLogger(LakeTable.class);

    public LakeTable(long id, String tableName, List<Column> baseSchema, KeysType keysType, PartitionInfo partitionInfo,
                     DistributionInfo defaultDistributionInfo, TableIndexes indexes) {
        super(id, tableName, baseSchema, keysType, partitionInfo, defaultDistributionInfo,
                GlobalStateMgr.getCurrentState().getClusterId(), indexes, TableType.LAKE);
    }

    public LakeTable(long id, String tableName, List<Column> baseSchema, KeysType keysType, PartitionInfo partitionInfo,
                     DistributionInfo defaultDistributionInfo) {
        this(id, tableName, baseSchema, keysType, partitionInfo, defaultDistributionInfo, null);
    }

    public String getStorageGroup() {
        return getShardStorageInfo().getObjectStorageInfo().getObjectUri();
    }

    public ShardStorageInfo getShardStorageInfo() {
        return tableProperty.getStorageInfo().getShardStorageInfo();
    }

    public void setStorageInfo(ShardStorageInfo shardStorageInfo, boolean enableCache, long cacheTtlS)
            throws DdlException {
        String storageGroup;
        // s3://bucket/serviceId/tableId/
        String path = String.format("%s/%d/", shardStorageInfo.getObjectStorageInfo().getObjectUri(), id);
        try {
            URI uri = new URI(path);
            String scheme = uri.getScheme();
            if (scheme == null) {
                throw new DdlException("Invalid storage path [" + path + "]: no scheme");
            }
            storageGroup = uri.normalize().toString();
        } catch (URISyntaxException e) {
            throw new DdlException("Invalid storage path [" + path + "]: " + e.getMessage());
        }

        ObjectStorageInfo objectStorageInfo =
                ObjectStorageInfo.newBuilder(shardStorageInfo.getObjectStorageInfo()).setObjectUri(storageGroup)
                        .build();
        ShardStorageInfo newShardStorageInfo =
                ShardStorageInfo.newBuilder(shardStorageInfo).setObjectStorageInfo(objectStorageInfo).build();

        if (tableProperty == null) {
            tableProperty = new TableProperty(new HashMap<>());
        }
        tableProperty
                .setStorageInfo(new StorageInfo(newShardStorageInfo, new StorageCacheInfo(enableCache, cacheTtlS)));
    }

    @Override
    public OlapTable selectiveCopy(Collection<String> reservedPartitions, boolean resetState,
                                   MaterializedIndex.IndexExtState extState) {
        LakeTable copied = DeepCopy.copyWithGson(this, LakeTable.class);
        if (copied == null) {
            LOG.warn("failed to copy lake table: {}", getName());
            return null;
        }
        return selectiveCopyInternal(copied, reservedPartitions, resetState, extState);
    }

    @Override
    public void write(DataOutput out) throws IOException {
        // write type first
        Text.writeString(out, type.name());
        Text.writeString(out, GsonUtils.GSON.toJson(this));
    }

    public static LakeTable read(DataInput in) throws IOException {
        // type is already read in Table
        String json = Text.readString(in);
        return GsonUtils.GSON.fromJson(json, LakeTable.class);
    }

    @Override
    public void onDrop(Database db, boolean force, boolean replay) {
        dropAllTempPartitions();
        for (long mvId : getRelatedMaterializedViews()) {
            Table tmpTable = db.getTable(mvId);
            if (tmpTable != null) {
                MaterializedView mv = (MaterializedView) tmpTable;
                mv.setActive(false);
            }
        }
    }

    @Override
    public Runnable delete(boolean replay) {
        GlobalStateMgr.getCurrentState().getLocalMetastore().onEraseTable(this);
        return replay ? null : new DeleteTableTask(this, replay);
    }

    private static class DeleteTableTask implements Runnable {
        private final LakeTable table;
        private final boolean replay;

        DeleteTableTask(LakeTable table, boolean replay) {
            this.table = table;
            this.replay = replay;
        }

        @Override
        public void run() {
            // inverted index
            TabletInvertedIndex invertedIndex = GlobalStateMgr.getCurrentInvertedIndex();
            Collection<Partition> allPartitions = table.getAllPartitions();
            for (Partition partition : allPartitions) {
                for (MaterializedIndex index : partition.getMaterializedIndices(MaterializedIndex.IndexExtState.ALL)) {
                    for (Tablet tablet : index.getTablets()) {
                        invertedIndex.deleteTablet(tablet.getId());
                    }
                }
            }

            if (replay) {
                return;
            }

            // drop all replicas
            Set<Long> tabletIds = new HashSet<>();
            for (Partition partition : table.getAllPartitions()) {
                List<MaterializedIndex> allIndices = partition.getMaterializedIndices(MaterializedIndex.IndexExtState.ALL);
                for (MaterializedIndex materializedIndex : allIndices) {
                    for (Tablet tablet : materializedIndex.getTablets()) {
                        tabletIds.add(tablet.getId());
                    }
                }
            }
            GlobalStateMgr.getCurrentState().getShardManager().getShardDeleter().addUnusedShardId(tabletIds);
            GlobalStateMgr.getCurrentState().getEditLog().logAddUnusedShard(tabletIds);
        }
    }
}
