/* 
   Copyright (c) 2012,2013 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef _ha_rocksdb_proto_h_
#define _ha_rocksdb_proto_h_

#include "rocksdb/table.h"

class Column_family_manager;

rocksdb::DB *rocksdb_get_rdb();
Column_family_manager& rocksdb_get_cf_manager();
rocksdb::BlockBasedTableOptions& rocksdb_get_table_options();
int rocksdb_normalize_tablename(const char *tablename,
                                StringBuffer<256> *strbuf);
int rocksdb_split_normalized_tablename(const char *fullname,
                                       StringBuffer<256> *dbbuf,
                                       StringBuffer<256> *tablebuf,
                                       StringBuffer<256> *partitionbuf);
std::vector<std::string> get_share_names(void);

/* Interface for retrieving performance counters */
enum {
  PC_KEY_SKIPPED_IDX= 0,
  PC_DELETE_SKIPPED_IDX,
  PC_MAX_IDX
};

typedef struct share_perf_counters {
  uint64_t value[PC_MAX_IDX];
} SHARE_PERF_COUNTERS;

int rocksdb_get_share_perf_counters(const char *tablename,
                                    SHARE_PERF_COUNTERS *counters);
#endif /* _ha_rocksdb_proto_h_ */
