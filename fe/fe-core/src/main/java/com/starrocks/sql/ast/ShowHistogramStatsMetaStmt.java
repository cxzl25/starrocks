// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.
package com.starrocks.sql.ast;

import com.google.common.collect.Lists;
import com.starrocks.analysis.Predicate;
import com.starrocks.analysis.RedirectStatus;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.ScalarType;
import com.starrocks.catalog.Table;
import com.starrocks.common.MetaNotFoundException;
import com.starrocks.privilege.PrivilegeManager;
import com.starrocks.qe.ConnectContext;
import com.starrocks.qe.ShowResultSetMetaData;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.statistic.HistogramStatsMeta;

import java.time.format.DateTimeFormatter;
import java.util.List;

public class ShowHistogramStatsMetaStmt extends ShowStmt {
    public ShowHistogramStatsMetaStmt(Predicate predicate) {
        setPredicate(predicate);
    }

    private static final ShowResultSetMetaData META_DATA =
            ShowResultSetMetaData.builder()
                    .addColumn(new Column("Database", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Table", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Column", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Type", ScalarType.createVarchar(20)))
                    .addColumn(new Column("UpdateTime", ScalarType.createVarchar(60)))
                    .addColumn(new Column("Properties", ScalarType.createVarchar(200)))
                    .build();

    public static List<String> showHistogramStatsMeta(ConnectContext context,
            HistogramStatsMeta histogramStatsMeta) throws MetaNotFoundException {
        List<String> row = Lists.newArrayList("", "", "", "", "", "");
        long dbId = histogramStatsMeta.getDbId();
        long tableId = histogramStatsMeta.getTableId();

        Database db = GlobalStateMgr.getCurrentState().getDb(dbId);
        if (db == null) {
            throw new MetaNotFoundException("No found database: " + dbId);
        }
        row.set(0, db.getOriginName());
        Table table = db.getTable(tableId);
        if (table == null) {
            throw new MetaNotFoundException("No found table: " + tableId);
        }
        // In new privilege framework(RBAC), user needs any action on the table to show analysis status for it.
        if (context.getGlobalStateMgr().isUsingNewPrivilege() &&
                !PrivilegeManager.checkAnyActionOnTable(context, db.getOriginName(), table.getName())) {
            return null;
        }
        row.set(1, table.getName());
        row.set(2, histogramStatsMeta.getColumn());
        row.set(3, histogramStatsMeta.getType().name());
        row.set(4, histogramStatsMeta.getUpdateTime().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")));
        row.set(5, histogramStatsMeta.getProperties() == null ? "{}" : histogramStatsMeta.getProperties().toString());

        return row;
    }

    @Override
    public ShowResultSetMetaData getMetaData() {
        return META_DATA;
    }

    @Override
    public RedirectStatus getRedirectStatus() {
        return RedirectStatus.FORWARD_NO_SYNC;
    }

    @Override
    public <R, C> R accept(AstVisitor<R, C> visitor, C context) {
        return visitor.visitShowHistogramStatsMetaStatement(this, context);
    }
}

