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
#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


#include "my_global.h"                   /* ulonglong */
#include "thr_lock.h"                    /* THR_LOCK, THR_LOCK_DATA */
#include "handler.h"                     /* handler */
#include "my_base.h"                     /* ha_rows */

#include "rocksdb/db.h"
#include "rocksdb/cache.h"
#include "rocksdb/comparator.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/statistics.h"
#include "rocksdb/perf_context.h"

#include "sql_string.h"

#include "rdb_applyiter.h"


/*
  This is
  - the name of the default Column Family (the CF which stores indexes which
    didn't explicitly specify which CF they are in)
  - the name used to set the default column family parameter for per-cf
    arguments.
*/
const char * const DEFAULT_CF_NAME= "default";

/*
  This is the name of the Column Family used for storing the data dictionary.
*/
const char * const DEFAULT_SYSTEM_CF_NAME= "__system__";

/*
  Column family name which means "put this index into its own column family".
  See get_per_index_cf_name.
*/
const char * const PER_INDEX_CF_NAME = "$per_index_cf";

inline bool looks_like_per_index_cf_typo(const char *name)
{
  return (name && name[0]=='$' && strcmp(name, PER_INDEX_CF_NAME));
}

extern PSI_stage_info stage_waiting_on_row_lock;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_mutex_dropped_indices_manager;
#endif

extern "C"
void thd_enter_cond(MYSQL_THD thd, mysql_cond_t *cond, mysql_mutex_t *mutex,
                    const PSI_stage_info *stage, PSI_stage_info *old_stage);
extern "C"
void thd_exit_cond(MYSQL_THD thd, const PSI_stage_info *stage);

/**
  Mark transaction to rollback and mark error as fatal to a sub-statement.
  @param  thd   Thread handle
  @param  all   TRUE <=> rollback main transaction.
*/
extern "C"
void thd_mark_transaction_to_rollback(MYSQL_THD thd, bool all);


//#endif

/** @brief
  ROCKSDB_SHARE is a structure that will be shared among all open handlers.
  This example implements the minimum of what you will probably need.

  //TODO: join this with RDBSE_TABLE_DEF ? 
*/
typedef struct st_rocksdb_share {
  char *table_name;
  uint table_name_length,use_count;
  //mysql_mutex_t mutex;

  THR_LOCK lock;

  /* Stores cumulative table statistics */
  my_io_perf_atomic_t io_perf_read;
  std::atomic_ullong internal_key_skipped_count;
  std::atomic_ullong internal_delete_skipped_count;
} ROCKSDB_SHARE;

typedef enum io_error_type {
  ROCKSDB_IO_ERROR_TRX_COMMIT,
  ROCKSDB_IO_ERROR_DICT_COMMIT,
  ROCKSDB_IO_ERROR_BG_THREAD
} ROCKSDB_IO_ERROR_TYPES;

void rocksdb_handle_io_error(rocksdb::Status status, enum io_error_type type);

class RDBSE_KEYDEF;
class RDBSE_TABLE_DEF;
class Row_lock;
class Rdb_transaction;

const char *const rocksdb_hton_name= "ROCKSDB";

/** @brief
  Class definition for the storage engine
*/

class ha_rocksdb: public handler
{
  THR_LOCK_DATA lock;      ///< MySQL lock
  ROCKSDB_SHARE *share;    ///< Shared lock info
  
  /* Iterator used for range scans and for full table/index scans */
  Apply_changes_iter *scan_it;

  RDBSE_TABLE_DEF *tbl_def;

  /* Primary Key encoder from KeyTupleFormat to StorageFormat */
  RDBSE_KEYDEF *pk_descr;

  /* Array of index descriptors */
  RDBSE_KEYDEF **key_descr;

  bool check_keyread_allowed(uint inx, uint part, bool all_parts) const;
  /* 
    Number of key parts in PK. This is the same as 
      table->key_info[table->s->primary_key].keyparts
  */
  uint pk_key_parts;
  
  //Rdb_transaction *cur_trx;

  /*
    TRUE <=> Primary Key columns can be decoded from the index
  */
  mutable bool pk_can_be_decoded;

  uchar *pk_tuple; /* Buffer for storing PK in KeyTupleFormat */
  uchar *pk_packed_tuple; /* Buffer for storing PK in StorageFormat */
  // ^^ todo: change it to 'char*'? TODO: ^ can we join this with last_rowkey?
  
  /*
    Temporary buffers for storing the key part of the Key/Value pair
    for secondary indexes.
  */
  uchar *sec_key_packed_tuple;

  /*
    Same as above, but it stores the value part. It holds unpack info which
    is currently not used (always empty).

    TODO: why does ha_rocksdb::open() assume that an upper bound of the size of
    this buffer is max_packed_sec_key_len? This is technically true currently, 
    but doesn't look meaningful.
  */
  uchar *sec_key_tails;
  
  /* Second buffers, used by UPDATE. */
  uchar *sec_key_packed_tuple_old;
  uchar *sec_key_tails_old;
  
  /*
    Temporary space for packing VARCHARs (we provide it to
    pack_record()/pack_index_tuple() calls).
  */
  uchar *pack_buffer;
 
  /* rowkey of the last record we've read, in StorageFormat. */
  String last_rowkey; 
  
  /* Buffer used by convert_record_to_storage_format() */
  String storage_record;

  /*
    Last retrieved record, in table->record[0] data format. It also serves as
    as storage for blob data (Field_blob object have pointers to here)

    TODO: Dont we lose one malloc() per record read by having it as std::string
    instead of rocksdb::Slice?
  */
  std::string retrieved_record;
  
  /* If TRUE, reads should place locks on rows */
  bool lock_rows;

  /* Lock that we've placed on the row we've just read. */
  Row_lock *last_row_lock;
 
  /* TRUE means we're doing an index-only read. FALSE means otherwise. */
  bool keyread_only;
  
  bool skip_scan_it_next_call;
  
  /*
    Valid inside UPDATE statements, when updated_indexes_is_valid=true: 

    This is a bitmap of indexes whose keys (in future, values) may be changed
    by this statement. Indexes that are not in the bitmap do not need to be
    updated.
  */
  key_map updated_indexes;
  bool updated_indexes_is_valid;

  /*
    MySQL index number for duplicate key error
  */
  int dupp_errkey;

  int create_key_defs(TABLE *table_arg, const char *db_table, uint len);
  int secondary_index_read(int keyno, uchar *buf);
  void setup_index_scan(RDBSE_KEYDEF *keydef, rocksdb::Slice *slice)
  {
    setup_index_scan(keydef, slice, false, false);
  }
  bool is_ascending(RDBSE_KEYDEF *keydef, enum ha_rkey_function find_flag);
  void setup_index_scan(RDBSE_KEYDEF *keydef, rocksdb::Slice *slice,
                        bool use_all_keys, bool is_ascending);
  int get_row_by_rowid(uchar *buf, const char *pk_tuple, uint pk_tuple_size);

  void update_auto_incr_val();
  void load_auto_incr_value();
  
  /* Idea copied from InnoDB's convert_error_code_to_mysql */
  int return_lock_failure(bool timed_out);
  
  /*
    Descriptor telling how to decode/encode a field to on-disk record storage
    format. Not all information is in the structure yet, but eventually we
    want to have as much as possible there to avoid virtual calls.

    For encoding/decoding of index tuples, see RDBSE_KEYDEF.
  */
  typedef struct st_field_encoder
  {
    /* 
      This is set to true for columns of Primary Key that can be decoded from
      their mem-comparable form.
      Since we can get them from the key part of RocksDB key->value pair, we
      don't need to store them in the value part.
    */
    bool dont_store;

    uint null_offset;
    uchar null_mask;  // 0 means the field cannot be null
    
    enum_field_types field_type;

    bool maybe_null() { return null_mask != 0; }
  } FIELD_ENCODER;
  
  /*
    Array of table->s->fields elements telling how to store fields in the
    record.
  */
  FIELD_ENCODER *field_enc;

  /*
    Number of bytes in on-disk (storage) record format that are used for
    storing SQL NULL flags.
  */
  uint null_bytes_in_rec;

  void setup_field_converters();
  
  // the buffer size should be at least 2*RDBSE_KEYDEF::INDEX_NUMBER_SIZE
  rocksdb::Range get_range(int i, uchar buf[]) const;

  /*
    Perf timers for data reads
   */
  struct {
    uint64_t block_read_byte;
    uint64_t block_read_count;
    uint64_t block_read_time;
    uint64_t internal_key_skipped_count;
    uint64_t internal_delete_skipped_count;
  } io_perf;
  void io_perf_start(void);
  void io_perf_end_and_record(void);

public:
  ha_rocksdb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_rocksdb() {}
  
  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const { return rocksdb_hton_name; }

  /* The following is only used by SHOW KEYS: */
  const char *index_type(uint inx) { return "BTREE"; }

  /** @brief
    The file extensions.
   */
  const char **bas_ext() const;

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const
  {
    /*
      HA_BINLOG_STMT_CAPABLE
        We are saying that this engine is just statement capable to have
        an engine that can only handle statement-based logging. This is
        used in testing.
      HA_REC_NOT_IN_SEQ 
        If we don't set it, filesort crashes, because it assumes rowids are 
        1..8 byte numbers 
    */
    return HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
           HA_REC_NOT_IN_SEQ | HA_CAN_INDEX_BLOBS |
           HA_REQUIRE_PRIMARY_KEY |
           (pk_can_be_decoded? HA_PRIMARY_KEY_IN_READ_INDEX:0) |
           HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
           HA_NULL_IN_KEY;
  }

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx, uint part, bool all_parts) const;

  const key_map * keys_to_use_for_scanning()
  {
    return &key_map_full;
  }
  bool primary_key_is_clustered()
  {
    return true;
  }

  int rename_table(const char *from, const char *to);

  int convert_record_from_storage_format(rocksdb::Slice *slice, uchar *buf);
  int convert_record_from_storage_format(uchar *buf);

  void convert_record_to_storage_format(rocksdb::Slice *packed_rec);

  /** @brief
    unireg.cc will call max_supported_record_length(), max_supported_keys(),
    max_supported_key_parts(), uint max_supported_key_length()
    to make sure that the storage engine can handle the data it is about to
    send. Return *real* limits of your storage engine here; MySQL will do
    min(your_limits, MySQL_limits) automatically.
   */
  uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }

  uint max_supported_keys()          const { return MAX_INDEXES; }
  uint max_supported_key_parts()     const { return MAX_REF_PARTS; }
  uint max_supported_key_part_length() const { return 2048; }
  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_length()    const { return 16*1024; /* just to return something*/ }
  
  /* At the moment, we're ok with default handler::index_init() implementation. */
  int index_read_map(uchar * buf, const uchar * key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag);
  
  int index_read_last_map(uchar * buf, const uchar * key, 
                          key_part_map keypart_map);

  virtual double scan_time() { return (double) (stats.records+stats.deleted) / 20.0+10; }
  virtual double read_time(uint, uint, ha_rows rows)
  { return (double) rows /  20.0+1; }

  int open(const char *name, int mode, uint test_if_locked);
  int close(void);

  int write_row(uchar *buf);
  int update_row(const uchar *old_data, uchar *new_data);
  int delete_row(const uchar *buf);

  int index_next(uchar *buf);
  int index_next_with_direction(uchar *buf, bool move_forward);
  int index_prev(uchar *buf);

  int index_first(uchar *buf);
  int index_last(uchar *buf);

  class Item* idx_cond_push(uint keyno, class Item* idx_cond);
  /*
    Default implementation from cancel_pushed_idx_cond() suits us
  */
private:
  int index_first_intern(uchar *buf);
  int index_last_intern(uchar *buf);

  enum icp_result check_index_cond();
  int find_icp_matching_index_rec(bool move_forward, uchar *buf);

  void calc_updated_indexes();
  int update_write_row(const uchar *old_data, uchar *new_data, bool do_locking);

  int read_key_exact(RDBSE_KEYDEF *kd, Apply_changes_iter *iter,
                     bool using_full_key, rocksdb::Slice& key_slice);
public:
  int index_end();

  void unlock_row();

  /** @brief
    Unlike index_init(), rnd_init() can be called two consecutive times
    without rnd_end() in between (it only makes sense if scan=1). In this
    case, the second call should prepare for the new table scan (e.g if
    rnd_init() allocates the cursor, the second call should position the
    cursor to the start of the table; no need to deallocate and allocate
    it again. This is a required method.
  */
  int rnd_init(bool scan);
  int rnd_end();

  int rnd_next(uchar *buf);
  int rnd_next_with_direction(uchar *buf, bool move_forward);

  int rnd_pos(uchar *buf, uchar *pos);
  void position(const uchar *record);
  int info(uint);
  int extra(enum ha_extra_function operation);
  int start_stmt(THD *thd, thr_lock_type lock_type);
  int external_lock(THD *thd, int lock_type);
  int delete_all_rows(ha_rows* nrows);
  int truncate();

  int reset()
  {
    /* Free blob data */
    retrieved_record.clear();
    return 0;
  }

  int check(THD* thd, HA_CHECK_OPT* check_opt);
  void remove_rows(RDBSE_TABLE_DEF *tbl);
  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key);
  int delete_table(const char *from);
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info);
  bool check_if_incompatible_data(HA_CREATE_INFO *info,
                                  uint table_changes);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);

  my_bool register_query_cache_table(THD *thd, char *table_key,
                                     uint key_length,
                                     qc_engine_callback
                                     *engine_callback,
                                     ulonglong *engine_data)
  {
    /* Currently, we don't support query cache */
    return FALSE;
  }
  bool get_error_message(int error, String *buf);

  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *first_value,
                          ulonglong *nb_reserved_values);
  void update_create_info(HA_CREATE_INFO *create_info);
  int optimize(THD *thd, HA_CHECK_OPT *check_opt);
};
