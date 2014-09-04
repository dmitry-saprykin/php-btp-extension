/*
 * Php extension to collect and send script statistical information to Btp daemon.
 *
 *  Author: raven
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "ext/standard/info.h"
#include "ext/standard/php_array.h"

#include "php_btp.h"

ZEND_DECLARE_MODULE_GLOBALS(btp)

#ifdef COMPILE_DL_BTP
ZEND_GET_MODULE(btp)
#endif

#define BTP_ONLY_STOPPED_TIMERS (1<<0)
#define BTP_DELETED_SOFT 1
#define BTP_DELETED_HARD 2
#define BTP_RESOURCE_NAME "Btp timer"
#define BTP_MS_IN_SEC 1000000

typedef struct _btp_timer_t {
  int rsrc_id;
  zend_bool started;
  zend_uchar deleted;
  unsigned short server_index;
  char *service;
  char *server;
  char *operation;
  char *script;
  int service_len;
  int server_len;
  int operation_len;
  int script_len;

  struct {
    int tv_sec;
    int tv_usec;
  } start;
  struct {
    zend_ulong64 tv_sec;
    int tv_usec;
  } value;
} btp_timer_t;

static int le_btp_timer;
static btp_timer_t* dummy_timer;

#define BTP_ZVAL_TO_TIMER(zval, timer) ZEND_FETCH_RESOURCE(timer, btp_timer_t *, &zval, -1, BTP_RESOURCE_NAME, le_btp_timer)

#define timeval_cvt(a, b) do { (a)->tv_sec = (b)->tv_sec; (a)->tv_usec = (b)->tv_usec; } while (0);
#define float_to_timeval(f, t) do { (t).tv_sec = (int)(f); (t).tv_usec = (int)((f - (double)(t).tv_sec) * 1000000.0); } while(0);
#define timeval_ms(t) ( 1000000UL * (t).tv_sec + (t).tv_usec )

static int btp_timer_release_helper(zend_rsrc_list_entry *le TSRMLS_DC);
static void btp_flush_data( int server_index TSRMLS_DC );

//----------------------------Common functions----------------------------------

static char* btp_itoa(zend_ulong64 val)
{
  static char buf[32] = {0};
  int i = 30;

  if(val == 0) {
    buf[i] = '0';
    --i;
  }
  else {
    for(; val && i ; --i, val /= 10) {
      buf[i] = "0123456789"[val % 10];
    }
  }

  return &buf[i+1];
}

static zend_bool btp_enabled( TSRMLS_D ) {
  if( BTP_G(in_rshutdown) ) {
    return 0;
  }

  if( BTP_G(is_cli) ) {
    return BTP_G(cli_enable);
  }

  return BTP_G(fpm_enable);
}

static void btp_autoflush( TSRMLS_D ) {
  zend_bool need_flush = 0;
  int i;

  if( BTP_G(autoflush_count) ) {
    unsigned int stopped_count = 0;
    for( i = 0; i < BTP_G(servers_used); i++) {
      stopped_count += BTP_G(servers)[i].stopped_count;
    }
    if( BTP_G(autoflush_count) <= stopped_count ) {
      need_flush = 1;
    }
  }

  if( !need_flush && BTP_G(autoflush_time) ) {
    time_t now = time(0);
    need_flush = (now >= (BTP_G(send_timer_start) + BTP_G(autoflush_time)));
    if( need_flush ) {
      BTP_G(send_timer_start) = now;
    }
  }

  if( need_flush ) {
    btp_flush_data(-1 TSRMLS_CC);
    zend_hash_apply(&EG(regular_list), (apply_func_t)btp_timer_release_helper TSRMLS_CC);
  }
}

static int btp_init_server (btp_server_t* server TSRMLS_DC)
{
  struct addrinfo *ai_list;
  struct addrinfo *ai_ptr;
  struct addrinfo  ai_hints;
  int fd;
  int status;

  memset(&ai_hints, 0, sizeof(ai_hints));
  ai_hints.ai_flags     = 0;
#ifdef AI_ADDRCONFIG
  ai_hints.ai_flags    |= AI_ADDRCONFIG;
#endif
  ai_hints.ai_family    = AF_UNSPEC;
  ai_hints.ai_socktype  = SOCK_DGRAM;
  ai_hints.ai_addr      = NULL;
  ai_hints.ai_canonname = NULL;
  ai_hints.ai_next      = NULL;

  ai_list = NULL;
  status = getaddrinfo(server->host, server->port, &ai_hints, &ai_list);
  if (status != 0) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp failed to resolve hostname '%s': %s", server->host, gai_strerror(status));
    return -1;
  }

  fd = -1;
  for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
    fd = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
    if (fd < 0) {
      continue;
    }

    memcpy( &server->sockaddr, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
    server->sockaddr_len = ai_ptr->ai_addrlen;
    break;
  }

  freeaddrinfo(ai_list);

  if( fd < 0 ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp connect failed failed %s:%s", server->host, server->port);
    return -1;
  }
  server->socket = fd;

  return 0;
}

static void btp_release_server (btp_server_t* server) {
  if( server->socket ) {
    close(server->socket);
    server->socket = 0;
  }
  if( server->sockaddr_len ) {
    memset(&server->sockaddr, 0, sizeof(server->sockaddr));
    server->sockaddr_len = 0;
  }
}

static btp_timer_t *btp_timer_ctor( zend_bool init_time TSRMLS_DC )
{
  struct timeval now;
  btp_timer_t *t;

  t = (btp_timer_t *)ecalloc(1, sizeof(btp_timer_t));
  t->started = 1;

  if( init_time ) {
    gettimeofday(&now, 0);
    timeval_cvt(&t->start, &now);
  }

  return t;
}

static inline int btp_timer_stop(btp_timer_t *t TSRMLS_DC)
{
  struct timeval now;
  struct rusage u, tmp;

  if ( !t->started ) {
    return FAILURE;
  }

  gettimeofday(&now, 0);
  timersub(&now, &t->start, &t->value);
  t->started = 0;
  BTP_G(servers)[t->server_index].stopped_count++;

#ifdef DEBUG2
  char *log_message = emalloc( 10000 );
  char *operation = estrndup(t->operation, t->operation_len);
  sprintf( log_message, "BTP: Timer stop: %s start: %i %i stop: %ld %ld  length: %i %i \n",
      operation,
      t->start.tv_sec, t->start.tv_usec,
      now.tv_sec, now.tv_usec,
      t->value.tv_sec, t->value.tv_usec
  );
  php_log_err( log_message TSRMLS_CC );
  efree(log_message);
  efree(operation);
#endif

  return SUCCESS;
}

static void btp_timer_dtor( btp_timer_t *t TSRMLS_DC )
{
  efree(t->operation);
  efree(t->server);
  efree(t->service);
  efree(t->script);

  t->operation = 0;
  t->server = 0;
  t->service = 0;
  t->script = 0;

  t->operation_len = 0;
  t->server_len = 0;
  t->service_len = 0;
  t->script_len = 0;
}

static void btp_timer_resource_dtor(zend_rsrc_list_entry *entry TSRMLS_DC)
{
  btp_timer_t *t = (btp_timer_t *)entry->ptr;
  btp_timer_dtor(t);
  efree(t);
}

static int btp_timer_dump_helper(zend_rsrc_list_entry *le, void *arg TSRMLS_DC)
{
  if (le->type == le_btp_timer) {
    btp_timer_t *t = (btp_timer_t *)le->ptr;
    if(t->rsrc_id != dummy_timer->rsrc_id) {
      zval *timer_list = (zval *)arg;
      zval *timer;

      ALLOC_INIT_ZVAL(timer);
      array_init(timer);

      add_assoc_long(timer, "server_id", BTP_G(servers)[t->server_index].id);
      add_assoc_stringl(timer, "server", t->server, t->server_len, 1);
      add_assoc_stringl(timer, "operation", t->operation, t->operation_len, 1);
      add_assoc_stringl(timer, "service", t->service, t->service_len, 1);
      if( t->script_len ) {
        add_assoc_stringl(timer, "script", t->script, t->script_len, 1);
      }
      add_assoc_bool(timer, "started", t->started);
      add_assoc_bool(timer, "deleted", t->deleted);
      add_assoc_long(timer, "start.sec", t->start.tv_sec);
      add_assoc_long(timer, "start.usec", t->start.tv_usec);

      if( !t->started ) {
        add_assoc_long(timer, "len.sec", t->value.tv_sec);
        add_assoc_long(timer, "len.usec", t->value.tv_usec);
      }

      add_next_index_zval(timer_list, timer);
    }
  }

  return ZEND_HASH_APPLY_KEEP;
}

static int btp_timer_stop_helper(zend_rsrc_list_entry *le TSRMLS_DC)
{
  if (le->type == le_btp_timer) {
    btp_timer_t *t = (btp_timer_t *)le->ptr;
    if( t->started ) {
      btp_timer_stop( t TSRMLS_CC );
    }
  }

  return ZEND_HASH_APPLY_KEEP;
}

static int btp_timer_delete_helper(zend_rsrc_list_entry *le TSRMLS_DC)
{
  if (le->type == le_btp_timer) {
    btp_timer_t *t = (btp_timer_t *)le->ptr;
    if( t->deleted != BTP_DELETED_HARD) {
      t->deleted = BTP_DELETED_HARD;
      if(le->refcount <= 1) {
        return ZEND_HASH_APPLY_REMOVE;
      }
      else {
        zend_list_delete(t->rsrc_id);
      }
    }
  }

  return ZEND_HASH_APPLY_KEEP;
}

static int btp_timer_release_helper(zend_rsrc_list_entry *le TSRMLS_DC)
{
  if (le->type == le_btp_timer) {
    btp_timer_t *t = (btp_timer_t *)le->ptr;

    if( t->deleted == BTP_DELETED_SOFT ) {
      t->deleted = BTP_DELETED_HARD;
      if(le->refcount <= 1) {
        return ZEND_HASH_APPLY_REMOVE;
      }
      else {
        zend_list_delete(t->rsrc_id);
      }
    }
  }

  return ZEND_HASH_APPLY_KEEP;
}

#ifdef NEWBTP

/*
{
  "jsonrpc":"2.0",
  "method":"publish",
  "params":{
    "channel":"btp2.rt",
    "content":[
       { "name":"service~~service1~~server1~~op1", "cl":[1,1,1]},
       { "name":"service~~service1~~server1~~op2", "cl":[2,2,2]},
       { "name":"service~~service2~~server1~~op1", "cl":[3,3,3]},
       { "name":"service~~service2~~server1~~op2", "cl":[4,4,4]},
       { "name":"service~~service1~~op1", "cl":[1,1,1]},
       { "name":"service~~service1~~op2", "cl":[2,2,2]},
       { "name":"service~~service2~~op1", "cl":[3,3,3]},
       { "name":"service~~service2~~op2", "cl":[4,4,4]},
       { "name":"script~~script1.phtml~~service1~~op1", "cl":[1,1,1]},
       { "name":"script~~script1.phtml~~service1~~op2", "cl":[2,2,2]},
       { "name":"script~~script1.phtml~~service2~~op1", "cl":[3,3,3]},
       { "name":"script~~script1.phtml~~service2~~op2", "cl":[4,4,4]}
    ]
  }
}
 */

#define HASH_PREALLOC 10

#define KEY_SERVICE "service~~"
#define KEY_SCRIPT "script~~"
#define SEPARATOR "~~"

#define OPENER "{\"jsonrpc\":\"2.0\",\"method\":\"publish\",\"params\":{\"channel\":\"btp2.rt\",\"content\":["
#define CLOSER "]}}\r\n"
#define ITEM_OPEN "{\"name\":\""
#define ITEM_CL "\",\"cl\":["
#define ITEM_CLOSE "]}"
#define COMMA ","

//собирает и удаляет счетчики для одного сервера
static int btp_timer_flush_helper(void *le TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key )
{
  zend_rsrc_list_entry *entry = le;

  if (entry->type == le_btp_timer) {
    zend_uint key_len;
    char *key, *offset;
    HashTable **temp;
    HashTable *timer_values;
    zend_ulong64 timer;

    HashTable *timers = va_arg(args, HashTable *);
    btp_server_t *server = va_arg(args, btp_server_t*);

    btp_timer_t *t = (btp_timer_t *)entry->ptr;
    if( BTP_G(servers)[t->server_index].id == server->id && !t->started && !t->deleted ) {
      t->deleted = BTP_DELETED_SOFT;
      timer = timeval_ms(t->value);

      //{ "name":"service~~service1~~server1~~op1", "cl":[1,1,1]},
      // выделим побольше памяти заранее, чтобы не перевыделять потом
      key_len = strlen(KEY_SERVICE) + (2 * strlen(SEPARATOR)) + t->service_len + t->server_len + t->operation_len;
      key = emalloc(key_len);
      offset = key;

      //{ "name":"service~~service1~~op1", "cl":[1,1,1]},
      key_len = strlen(KEY_SERVICE) + strlen(SEPARATOR) + t->service_len + t->operation_len;

      memcpy( offset, KEY_SERVICE, strlen(KEY_SERVICE) );
      offset += strlen(KEY_SERVICE);

      memcpy( offset, t->service, t->service_len );
      offset += t->service_len;

      memcpy( offset, SEPARATOR, strlen(SEPARATOR) );
      offset += strlen(SEPARATOR);

      memcpy( offset, t->operation, t->operation_len );

      if ( zend_hash_find(timers, key, key_len, (void**)&temp) != SUCCESS ) {
        ALLOC_HASHTABLE(timer_values);
        zend_hash_init(timer_values, HASH_PREALLOC, NULL, NULL, 0);
        zend_hash_add(timers, key, key_len, &timer_values, sizeof(HashTable *), NULL);
      }
      else {
        timer_values = (*temp);
      }

      zend_hash_next_index_insert(timer_values, &timer, sizeof(zend_ulong64), NULL);

      //{ "name":"service~~service1~~server1~~op1", "cl":[1,1,1]},
      key_len = strlen(KEY_SERVICE) + 2 * strlen(SEPARATOR) + t->service_len + t->server_len + t->operation_len;

      memcpy( offset, t->server, t->server_len );
      offset += t->server_len;

      memcpy( offset, SEPARATOR, strlen(SEPARATOR) );
      offset += strlen(SEPARATOR);

      memcpy( offset, t->operation, t->operation_len );

      if ( zend_hash_find(timers, key, key_len, (void**)&temp) != SUCCESS ) {
        ALLOC_HASHTABLE(timer_values);
        zend_hash_init(timer_values, HASH_PREALLOC, NULL, NULL, 0);
        zend_hash_add(timers, key, key_len, &timer_values, sizeof(HashTable *), NULL);
      }
      else {
        timer_values = (*temp);
      }

      zend_hash_next_index_insert(timer_values, &timer, sizeof(zend_ulong64), NULL);

      offset = 0;
      efree(key);

      // { "name":"script~~script1.phtml~~service1~~op1", "cl":[1,1,1]},
      if( BTP_G(script_name) || t->script_len ) {

        char *script;
        int script_len;
        if( t->script_len ) {
          script_len = t->script_len;
          script = t->script;
        }
        else {
          script_len = BTP_G(script_name_len);
          script = BTP_G(script_name);
        }

        key_len = strlen(KEY_SCRIPT) + 2 * strlen(SEPARATOR) + script_len + t->service_len + t->operation_len;
        key = emalloc(key_len);
        offset = key;

        memcpy( offset, KEY_SCRIPT, strlen(KEY_SCRIPT) );
        offset += strlen(KEY_SCRIPT);

        memcpy( offset, script, script_len );
        offset += script_len;

        memcpy( offset, SEPARATOR, strlen(SEPARATOR) );
        offset += strlen(SEPARATOR);

        memcpy( offset, t->service, t->service_len );
        offset += t->service_len;

        memcpy( offset, SEPARATOR, strlen(SEPARATOR) );
        offset += strlen(SEPARATOR);

        memcpy( offset, t->operation, t->operation_len );

        if ( zend_hash_find(timers, key, key_len, (void**)&temp) != SUCCESS ) {
          ALLOC_HASHTABLE(timer_values);
          zend_hash_init(timer_values, HASH_PREALLOC, NULL, NULL, 0);
          zend_hash_add(timers, key, key_len, &timer_values, sizeof(HashTable *), NULL);
        }
        else {
          timer_values = (*temp);
        }

        zend_hash_next_index_insert(timer_values, &timer, sizeof(zend_ulong64), NULL);

        offset = 0;
        efree(key);
      }
    }
  }

  return ZEND_HASH_APPLY_KEEP;
}

static size_t btp_request_size(HashTable *timers TSRMLS_DC ) {
  HashPosition timers_pointer, timer_values_pointer;
  HashTable *timer_values;
  HashTable **temp;

  char *key;
  int key_len;
  long index;
  zend_ulong64 *timer;
  char *timer_str;

  size_t result = 0;

  result += strlen(OPENER);

  for(
    zend_hash_internal_pointer_reset_ex(timers, &timers_pointer);
    zend_hash_get_current_data_ex(timers, (void **) &temp, &timers_pointer) == SUCCESS;
    zend_hash_move_forward_ex(timers, &timers_pointer)
  ) {
    timer_values = (*temp);
    zend_hash_get_current_key_ex(timers, &key, &key_len, &index, 0, &timers_pointer);
    result += strlen(ITEM_OPEN) + key_len + strlen(ITEM_CL);

    for(
      zend_hash_internal_pointer_reset_ex(timer_values, &timer_values_pointer);
      zend_hash_get_current_data_ex(timer_values, (void **) &timer, &timer_values_pointer) == SUCCESS;
      zend_hash_move_forward_ex(timer_values, &timer_values_pointer)
    ) {
      timer_str = btp_itoa(*timer);
      result += strlen(timer_str) + strlen(COMMA);
    }

    result += strlen(ITEM_CLOSE);
  }

  result -= strlen(COMMA);
  result += strlen(CLOSER);

  return result;
}

static void btp_request_data(HashTable *timers, char *request TSRMLS_DC ) {
  HashPosition timers_pointer, timer_values_pointer;
  HashTable *timer_values;
  HashTable **temp;

  char *key;
  int key_len;
  long index;
  zend_ulong64 *timer;
  char *timer_str;

  memcpy(request, OPENER, strlen(OPENER));
  request += strlen(OPENER);

  for(
    zend_hash_internal_pointer_reset_ex(timers, &timers_pointer);
    zend_hash_get_current_data_ex(timers, (void **) &temp, &timers_pointer) == SUCCESS;
    zend_hash_move_forward_ex(timers, &timers_pointer)
  ) {
    timer_values = (*temp);
    zend_hash_get_current_key_ex(timers, &key, &key_len, &index, 0, &timers_pointer);

    memcpy(request, ITEM_OPEN, strlen(ITEM_OPEN));
    request += strlen(ITEM_OPEN);

    memcpy(request, key, key_len);
    request += key_len;

    memcpy(request, ITEM_CL, strlen(ITEM_CL));
    request += strlen(ITEM_CL);

    for(
      zend_hash_internal_pointer_reset_ex(timer_values, &timer_values_pointer);
      zend_hash_get_current_data_ex(timer_values, (void **) &timer, &timer_values_pointer) == SUCCESS;
      zend_hash_move_forward_ex(timer_values, &timer_values_pointer)
    ) {
      timer_str = btp_itoa(*timer);

      memcpy(request, timer_str, strlen(timer_str));
      request += strlen(timer_str);

      memcpy(request, COMMA, strlen(COMMA));
      request += strlen(COMMA);
    }

    request -= strlen(COMMA);

    memcpy(request, ITEM_CLOSE, strlen(ITEM_CLOSE));
    request += strlen(ITEM_CLOSE);

    memcpy(request, COMMA, strlen(COMMA));
    request += strlen(COMMA);
  }

  request -= strlen(COMMA);

  memcpy(request, CLOSER, strlen(CLOSER));
  request += strlen(CLOSER);
}

static void btp_request_free_hash(HashTable *timers TSRMLS_DC ) {
  HashPosition timers_pointer;
  HashTable *timer_values;
  HashTable **temp;

  for(
    zend_hash_internal_pointer_reset_ex(timers, &timers_pointer);
    zend_hash_get_current_data_ex(timers, (void **) &temp, &timers_pointer) == SUCCESS;
    zend_hash_move_forward_ex(timers, &timers_pointer)
  ) {
    timer_values = (*temp);
    zend_hash_destroy(timer_values);
    FREE_HASHTABLE(timer_values);
  }
}

#else

/*
{"jsonrpc":"2.0","method":"put","params":
  [
    {"script":"/diary/post.phtml","items":{"Nginx":{"wwwnew34":{"302":[500]}}}},
    {"script":"other_script","items":{"Nginx":{"wwwnew34":{"302":[500]}}}},
  ]
}
 */
#define OPENER "{\"jsonrpc\":\"2.0\",\"method\":\"put\",\"params\":["
#define SCRIPT_OPENER "{\"script\":\""
#define ITEMS "\",\"items\":{"
#define OPEN_CURLY "\":{"
#define CLOSE_CURLY "}"
#define OPEN_SQUARE "\":["
#define CLOSE_SQUARE "]"
#define COMMA ","
#define DOUBLE_TICK "\""
#define CLOSER "]}\r\n"

static void btp_get_script_by_timer(btp_timer_t *t, char **script, int *script_len TSRMLS_DC)
{
  if( t->script_len ) {
    *script = t->script;
    *script_len = t->script_len;
  }
  else if( BTP_G(script_name_len) ) {
    *script = BTP_G(script_name);
    *script_len = BTP_G(script_name_len);
  }
  else {
    *script = "";
    *script_len = 0;
  }
}

//собирает и удаляет счетчики для одного сервера
static int btp_timer_flush_helper(void *le TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key )
{
  zend_rsrc_list_entry *entry = le;
  if (entry->type == le_btp_timer) {
    HashTable **temp;
    HashTable *timers = va_arg(args, HashTable *);
    btp_server_t *server = va_arg(args, btp_server_t *);


    btp_timer_t *t = (btp_timer_t *)entry->ptr;
    if( BTP_G(servers)[t->server_index].id == server->id && !t->started && !t->deleted ) {
      t->deleted = BTP_DELETED_SOFT;

      char *script;
      int script_len;
      btp_get_script_by_timer(t, &script, &script_len TSRMLS_CC);

      HashTable *script_hash = NULL;
      if ( zend_hash_find(timers, script, script_len, (void**)&temp) != SUCCESS ) {
        ALLOC_HASHTABLE(script_hash);
        zend_hash_init(script_hash, 1, NULL, NULL, 0);
        zend_hash_add(timers, script, script_len, &script_hash, sizeof(HashTable *), NULL);
      }
      else {
        script_hash = (*temp);
      }

      HashTable *service_hash = NULL;
      if ( zend_hash_find(script_hash, t->service, t->service_len, (void**)&temp) != SUCCESS ) {
        ALLOC_HASHTABLE(service_hash);
        zend_hash_init(service_hash, zend_hash_num_elements(timers), NULL, NULL, 0);
        zend_hash_add(script_hash, t->service, t->service_len, &service_hash, sizeof(HashTable *), NULL);
      }
      else {
        service_hash = (*temp);
      }

      HashTable *server_hash = NULL;
      if ( zend_hash_find(service_hash, t->server, t->server_len, (void**)&temp) != SUCCESS ) {
        ALLOC_HASHTABLE(server_hash);
        zend_hash_init(server_hash, zend_hash_num_elements(timers), NULL, NULL, 0);
        zend_hash_add(service_hash, t->server, t->server_len, &server_hash, sizeof(HashTable *), NULL);
      }
      else {
        server_hash = (*temp);
      }

      HashTable *operation_hash = NULL;
      if ( zend_hash_find(server_hash, t->operation, t->operation_len, (void**)&temp) != SUCCESS ) {
        ALLOC_HASHTABLE(operation_hash);
        zend_hash_init(operation_hash, zend_hash_num_elements(timers), NULL, NULL, 0);
        zend_hash_add(server_hash, t->operation, t->operation_len, &operation_hash, sizeof(HashTable *), NULL);
      }
      else {
        operation_hash = (*temp);
      }

      zend_ulong64 timer = timeval_ms(t->value);
      zend_hash_next_index_insert(operation_hash, &timer, sizeof(zend_ulong64), NULL);
    }
  }

  return ZEND_HASH_APPLY_KEEP;
}

static size_t btp_request_size(HashTable *timers TSRMLS_DC ) {
  HashPosition timers_pointer, script_pointer, service_pointer, server_pointer, operation_pointer;
  HashTable **temp;

  char *key;
  int key_len;
  long index;

  size_t result = 0;

  result += strlen(OPENER);

  HashTable *script_hash;
  for(
    zend_hash_internal_pointer_reset_ex(timers, &timers_pointer);
    zend_hash_get_current_data_ex(timers, (void **) &temp, &timers_pointer) == SUCCESS;
    zend_hash_move_forward_ex(timers, &timers_pointer)
  ) {

    script_hash = (*temp);
    zend_hash_get_current_key_ex(timers, &key, &key_len, &index, 0, &timers_pointer);

    result += strlen(SCRIPT_OPENER) + key_len  + strlen(ITEMS);

    HashTable *service_hash;
    for(
      zend_hash_internal_pointer_reset_ex(script_hash, &script_pointer);
      zend_hash_get_current_data_ex(script_hash, (void **) &temp, &script_pointer) == SUCCESS;
      zend_hash_move_forward_ex(script_hash, &script_pointer)
    ) {

      service_hash = (*temp);
      zend_hash_get_current_key_ex(script_hash, &key, &key_len, &index, 0, &script_pointer);
      result += strlen(DOUBLE_TICK) + key_len + strlen(OPEN_CURLY);

      HashTable *server_hash;
      for(
        zend_hash_internal_pointer_reset_ex(service_hash, &service_pointer);
        zend_hash_get_current_data_ex(service_hash, (void **) &temp, &service_pointer) == SUCCESS;
        zend_hash_move_forward_ex(service_hash, &service_pointer)
      ) {
        server_hash = (*temp);
        zend_hash_get_current_key_ex(service_hash, &key, &key_len, &index, 0, &service_pointer);
        result += strlen(DOUBLE_TICK) + key_len + strlen(OPEN_CURLY);

        HashTable *operation_hash;
        for(
         zend_hash_internal_pointer_reset_ex(server_hash, &server_pointer);
         zend_hash_get_current_data_ex(server_hash, (void **) &temp, &server_pointer) == SUCCESS;
         zend_hash_move_forward_ex(server_hash, &server_pointer)
        ) {
          operation_hash = (*temp);
          zend_hash_get_current_key_ex(server_hash, &key, &key_len, &index, 0, &server_pointer);
          result += strlen(DOUBLE_TICK) + key_len + strlen(OPEN_SQUARE);

          zend_ulong64 *timer;
          for(
            zend_hash_internal_pointer_reset_ex(operation_hash, &operation_pointer);
            zend_hash_get_current_data_ex(operation_hash, (void **) &timer, &operation_pointer) == SUCCESS;
            zend_hash_move_forward_ex(operation_hash, &operation_pointer)
          ) {
            char *timer_str = btp_itoa(*timer);
            result += strlen(timer_str) + strlen(COMMA);
          }

          result -= strlen(COMMA);
          result += strlen(CLOSE_SQUARE) + strlen(COMMA);
        }

        result -= strlen(COMMA);
        result += strlen(CLOSE_CURLY) + strlen(COMMA);
      }

      result -= strlen(COMMA);
      result += strlen(CLOSE_CURLY) + strlen(COMMA);
    }

    result -= strlen(COMMA);
    result += strlen(CLOSE_CURLY) + strlen(COMMA);
  }

  result -= strlen(COMMA);
  result += strlen(CLOSER);

  return result;
}

static void btp_request_data(HashTable *timers, char *request TSRMLS_DC ) {
  HashPosition timers_pointer, service_pointer, server_pointer, operation_pointer, script_pointer;
  HashTable **temp;
  btp_timer_t **data;
  btp_timer_t *timer;
  char *ival;
  char *key;
  int key_len;
  long index;

  memcpy(request, OPENER, strlen(OPENER));
  request += strlen(OPENER);

  HashTable *script_hash;
  for(
    zend_hash_internal_pointer_reset_ex(timers, &timers_pointer);
    zend_hash_get_current_data_ex(timers, (void **) &temp, &timers_pointer) == SUCCESS;
    zend_hash_move_forward_ex(timers, &timers_pointer)
  ) {

    script_hash = (*temp);
    zend_hash_get_current_key_ex(timers, &key, &key_len, &index, 0, &timers_pointer);

    memcpy(request, SCRIPT_OPENER, strlen(SCRIPT_OPENER));
    request += strlen(SCRIPT_OPENER);

    memcpy(request, key, key_len);
    request += key_len;

    memcpy(request, ITEMS, strlen(ITEMS));
    request += strlen(ITEMS);

    HashTable *service_hash;
    for(
      zend_hash_internal_pointer_reset_ex(script_hash, &script_pointer);
      zend_hash_get_current_data_ex(script_hash, (void **) &temp, &script_pointer) == SUCCESS;
      zend_hash_move_forward_ex(script_hash, &script_pointer)
    ) {
      service_hash = (*temp);
      zend_hash_get_current_key_ex(script_hash, &key, &key_len, &index, 0, &script_pointer);

      memcpy(request, DOUBLE_TICK, strlen(DOUBLE_TICK));
      request += strlen(DOUBLE_TICK);

      memcpy(request, key, key_len);
      request += key_len;

      memcpy(request, OPEN_CURLY, strlen(OPEN_CURLY));
      request += strlen(OPEN_CURLY);

      HashTable *server_hash;
      for(
        zend_hash_internal_pointer_reset_ex(service_hash, &service_pointer);
        zend_hash_get_current_data_ex(service_hash, (void **) &temp, &service_pointer) == SUCCESS;
        zend_hash_move_forward_ex(service_hash, &service_pointer)
      ) {
        server_hash = (*temp);
        zend_hash_get_current_key_ex(service_hash, &key, &key_len, &index, 0, &service_pointer);

        memcpy(request, DOUBLE_TICK, strlen(DOUBLE_TICK));
        request += strlen(DOUBLE_TICK);

        memcpy(request, key, key_len);
        request += key_len;

        memcpy(request, OPEN_CURLY, strlen(OPEN_CURLY));
        request += strlen(OPEN_CURLY);

        HashTable *operation_hash;
        for(
         zend_hash_internal_pointer_reset_ex(server_hash, &server_pointer);
         zend_hash_get_current_data_ex(server_hash, (void **) &temp, &server_pointer) == SUCCESS;
         zend_hash_move_forward_ex(server_hash, &server_pointer)
        ) {
          operation_hash = (*temp);
          zend_hash_get_current_key_ex(server_hash, &key, &key_len, &index, 0, &server_pointer);

          memcpy(request, DOUBLE_TICK, strlen(DOUBLE_TICK));
          request += strlen(DOUBLE_TICK);

          memcpy(request, key, key_len);
          request += key_len;

          memcpy(request, OPEN_SQUARE, strlen(OPEN_SQUARE));
          request += strlen(OPEN_SQUARE);

          zend_ulong64 *timer;
          for(
            zend_hash_internal_pointer_reset_ex(operation_hash, &operation_pointer);
            zend_hash_get_current_data_ex(operation_hash, (void **) &timer, &operation_pointer) == SUCCESS;
            zend_hash_move_forward_ex(operation_hash, &operation_pointer)
          ) {
            char *timer_str = btp_itoa(*timer);

            memcpy(request, timer_str, strlen(timer_str));
            request += strlen(timer_str);

            memcpy(request, COMMA, strlen(COMMA));
            request += strlen(COMMA);
          }

          request -= strlen(COMMA);

          memcpy(request, CLOSE_SQUARE, strlen(CLOSE_SQUARE));
          request += strlen(CLOSE_SQUARE);

          memcpy(request, COMMA, strlen(COMMA));
          request += strlen(COMMA);
        }

        request -= strlen(COMMA);

        memcpy(request, CLOSE_CURLY, strlen(CLOSE_CURLY));
        request += strlen(CLOSE_CURLY);

        memcpy(request, COMMA, strlen(COMMA));
        request += strlen(COMMA);
      }

      request -= strlen(COMMA);

      memcpy(request, CLOSE_CURLY, strlen(CLOSE_CURLY));
      request += strlen(CLOSE_CURLY);

      memcpy(request, COMMA, strlen(COMMA));
      request += strlen(COMMA);
    }

    request -= strlen(COMMA);

    memcpy(request, CLOSE_CURLY, strlen(CLOSE_CURLY));
    request += strlen(CLOSE_CURLY);

    memcpy(request, COMMA, strlen(COMMA));
    request += strlen(COMMA);
  }

  request -= strlen(COMMA);

  memcpy(request, CLOSER, strlen(CLOSER));
  request += strlen(CLOSER);
}

static void btp_request_free_hash(HashTable *timers TSRMLS_DC ) {
  HashPosition timers_pointer, service_pointer, server_pointer, script_pointer;
  HashTable *script_hash;
  HashTable *service_hash;
  HashTable *server_hash;
  HashTable *operation_hash;
  HashTable **temp;

  for(
    zend_hash_internal_pointer_reset_ex(timers, &timers_pointer);
    zend_hash_get_current_data_ex(timers, (void **) &temp, &timers_pointer) == SUCCESS;
    zend_hash_move_forward_ex(timers, &timers_pointer)
  ) {
    script_hash = (*temp);

    for(
      zend_hash_internal_pointer_reset_ex(script_hash, &script_pointer);
      zend_hash_get_current_data_ex(script_hash, (void **) &temp, &script_pointer) == SUCCESS;
      zend_hash_move_forward_ex(script_hash, &script_pointer)
    ) {
      service_hash = (*temp);

      for(
        zend_hash_internal_pointer_reset_ex(service_hash, &service_pointer);
        zend_hash_get_current_data_ex(service_hash, (void **) &temp, &service_pointer) == SUCCESS;
        zend_hash_move_forward_ex(service_hash, &service_pointer)
      ) {
        server_hash = (*temp);

        for(
         zend_hash_internal_pointer_reset_ex(server_hash, &server_pointer);
         zend_hash_get_current_data_ex(server_hash, (void **) &temp, &server_pointer) == SUCCESS;
         zend_hash_move_forward_ex(server_hash, &server_pointer)
        ) {
          operation_hash = (*temp);
          zend_hash_destroy(operation_hash);
          FREE_HASHTABLE(operation_hash);
        }

        zend_hash_destroy(server_hash);
        FREE_HASHTABLE(server_hash);
      }

      zend_hash_destroy(service_hash);
      FREE_HASHTABLE(service_hash);
    }

    zend_hash_destroy(script_hash);
    FREE_HASHTABLE(script_hash);
  }
}

#endif

static void btp_request_send( char *request, size_t request_size, btp_server_t *server TSRMLS_DC ) {
  if( server->socket > 0) {
    size_t sent, total_sent = 0;
    while (total_sent < request_size) {
      int flags = 0;

      sent = sendto(server->socket, request + total_sent, request_size - total_sent, flags,
          (struct sockaddr *) &server->sockaddr, server->sockaddr_len);
      if (sent < 0) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp send failed to %s:%s", server->host, server->port);
        return;
      }
      total_sent += sent;
    }
  }
}

static void btp_flush_data_server( btp_server_t* server TSRMLS_DC ) {
  if( server->stopped_count < 1) {
    return;
  }

  HashTable *timers;
  size_t request_size;
  char *request;

  ALLOC_HASHTABLE(timers);
  zend_hash_init(timers, server->stopped_count, NULL, NULL, 0);
  zend_hash_apply_with_arguments(&EG(regular_list), btp_timer_flush_helper, 2, timers, server );

  request_size = btp_request_size(timers TSRMLS_CC);
  request = emalloc(request_size + 1);
  btp_request_data(timers, request TSRMLS_CC);

#ifdef DEBUG1
  request[request_size] = '\0';
  //php_error_docref(NULL TSRMLS_CC, E_NOTICE, "BTP: Request size: %zu\n Request data: %s", request_size, request);
  char *log_message = emalloc(request_size + 1000);
  sprintf( log_message, "BTP: Request size: %zu\n Request data: %s", request_size, request );
  php_log_err( log_message TSRMLS_CC );
  efree(log_message);
#endif

  btp_request_send(request, request_size, server TSRMLS_CC);
  efree(request);

  btp_request_free_hash(timers);
  zend_hash_destroy(timers);
  FREE_HASHTABLE(timers);

  server->stopped_count = 0;
}

//отправляет в btp все остановленные счетчики
static void btp_flush_data( int server_index TSRMLS_DC )
{
  if( server_index == -1) {
    int i;
    for(i = 0; i < BTP_G(servers_used); i++) {
      btp_flush_data_server( &BTP_G(servers)[i] TSRMLS_CC);
    }
  }
  else {
    btp_flush_data_server( &BTP_G(servers)[server_index] TSRMLS_CC);
  }
}

//----------------------------Extension functions------------------------------------

//proto bool btp_config_server_set(int id, string host, int port)
static PHP_FUNCTION(btp_config_server_set)
{
  char *host;
  char *port;
  long server_id;
  int host_len, port_len;
  server_id = 0;
  host_len = 0;
  port_len = 0;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lss", &server_id, &host, &host_len, &port, &port_len) != SUCCESS) {
    RETURN_FALSE;
  }

  if( host_len > SERVER_HOST_MAXLEN ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp host is too long (max %i, got %ld)!", SERVER_HOST_MAXLEN, host_len);
    RETURN_FALSE;
  }

  if( server_id < INT_MIN || server_id > INT_MAX ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp server_id is not valid!");
    RETURN_FALSE;
  }

  if( port_len > SERVER_PORT_MAXLEN ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp port is not valid!");
    RETURN_FALSE;
  }

  btp_server_t server;
  memset(&server, 0, sizeof(server));

  server.id = server_id;
  memcpy(server.host, host, host_len);
  server.host[host_len] = '\0';

  memcpy(server.port, port, port_len);
  server.port[port_len] = '\0';

  if( btp_init_server(&server TSRMLS_CC) != 0) {
    RETURN_FALSE;
  }

  int i;
  for(i = 0; i < BTP_G(servers_used); i++) {
    if( BTP_G(servers)[i].id == server.id ) {
        btp_release_server(&BTP_G(servers)[i]);
        BTP_G(servers)[i] = server;
        RETURN_TRUE;
    }
  }

  if( BTP_G(servers_used) >= SERVER_LIST_MAXLEN ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp server list is full (maxlen %i)", SERVER_LIST_MAXLEN);
    RETURN_FALSE;
  }

  BTP_G(servers)[ BTP_G(servers_used) ] = server;
  BTP_G(servers_used)++;

  RETURN_TRUE;
}

//proto bool btp_dump()
static PHP_FUNCTION( btp_dump )
{
  zval *server, *server_list, *timer_list;
  int i;

  array_init(return_value);

  if( BTP_G(script_name) ) {
    add_assoc_stringl(return_value, "script_name", BTP_G(script_name), BTP_G(script_name_len), 1);
  }

  add_assoc_bool(return_value, "is_cli", BTP_G(is_cli) );
  add_assoc_long(return_value, "send_timer_start", BTP_G(send_timer_start));

  //servers
  ALLOC_INIT_ZVAL(server_list);
  array_init(server_list);
  for( i = 0; i < BTP_G(servers_used); i++) {
    ALLOC_INIT_ZVAL(server);
    array_init(server);

    btp_server_t* btp_server = &BTP_G(servers)[i];

    add_assoc_long(server, "id", btp_server->id);
    add_assoc_string(server, "host", btp_server->host, 1);
    add_assoc_string(server, "port", btp_server->port, 1);
    add_assoc_long(server, "stopped_count", btp_server->stopped_count);
    add_assoc_bool(server, "is_connected", btp_server->socket > 0);

    add_next_index_zval(server_list, server);
  }
  add_assoc_zval(return_value, "servers", server_list);

  ALLOC_INIT_ZVAL(timer_list);
  array_init(timer_list);
  zend_hash_apply_with_argument(&EG(regular_list), (apply_func_arg_t) btp_timer_dump_helper, (void *)timer_list TSRMLS_CC);
  add_assoc_zval(return_value, "timers", timer_list);
}

//proto bool btp_dump_timer(resource timer)
static PHP_FUNCTION( btp_dump_timer )
{
  zval *timer;
  btp_timer_t *t;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &timer) != SUCCESS) {
    RETURN_FALSE;
  }

  BTP_ZVAL_TO_TIMER(timer, t);

  //попытка получить таймер, созданный когда btp был отключен
  if( t->rsrc_id == dummy_timer->rsrc_id ) {
    RETURN_FALSE;
  }

  array_init(return_value);
  add_assoc_long(return_value, "server_id", BTP_G(servers)[t->server_index].id);
  add_assoc_stringl(return_value, "server", t->server, t->server_len, 1);
  add_assoc_stringl(return_value, "operation", t->operation, t->operation_len, 1);
  add_assoc_stringl(return_value, "service", t->service, t->service_len, 1);
  if( t->script_len ) {
    add_assoc_stringl(return_value, "script", t->script, t->script_len, 1);
  }
  add_assoc_bool(return_value, "started", t->started);
  add_assoc_bool(return_value, "deleted", t->deleted);
  add_assoc_long(return_value, "start.sec", t->start.tv_sec);
  add_assoc_long(return_value, "start.usec", t->start.tv_usec);

  if( !t->started ) {
    add_assoc_long(return_value, "len.sec", t->value.tv_sec);
    add_assoc_long(return_value, "len.usec", t->value.tv_usec);
  }
}

//proto bool btp_script_name_set(string script_name)
static PHP_FUNCTION( btp_script_name_set )
{
  char *script_name;
  int script_name_len;
  if ( zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &script_name, &script_name_len ) != SUCCESS ) {
    RETURN_FALSE;
  }

  if ( BTP_G(script_name) ) {
    efree(BTP_G(script_name));
  }

  BTP_G(script_name) = estrndup(script_name, script_name_len);
  BTP_G(script_name_len) = script_name_len;

  RETURN_TRUE;
}

//proto resource btp_timer_start(string service, string server, string operation, int server_id = 0)
static PHP_FUNCTION( btp_timer_start )
{
  if( !btp_enabled( TSRMLS_C) ) {
    zend_list_addref( dummy_timer->rsrc_id );
    RETURN_RESOURCE( dummy_timer->rsrc_id );
  }

  btp_timer_t *t = NULL;
  char *service, *server, *operation;
  int service_len, server_len, operation_len;
  long server_id;
  short i, server_index;

  server_id = 0;
  if ( zend_parse_parameters(
      ZEND_NUM_ARGS() TSRMLS_CC, "sss|l", &service, &service_len, &server, &server_len, &operation, &operation_len, &server_id
    ) != SUCCESS
  ) {
    RETURN_FALSE;
  }

  server_index = -1;
  for(i = 0; i < BTP_G(servers_used); i++) {
    if( BTP_G(servers)[i].id == server_id ) {
      server_index = i;
      break;
    }
  }

  if( server_index == -1 ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp server id %ld is not found!", server_id);
    RETURN_FALSE;
  }

  t = btp_timer_ctor( 1 TSRMLS_CC);
  t->service = estrndup(service, service_len);
  t->service_len = service_len;
  t->server = estrndup(server, server_len);
  t->server_len = server_len;
  t->operation = estrndup(operation, operation_len);
  t->operation_len = operation_len;
  t->server_index = server_index;
  t->rsrc_id = zend_list_insert(t, le_btp_timer TSRMLS_CC);


  /* refcount++ so that the timer is shut down only on request finish if not stopped manually */
  zend_list_addref( t->rsrc_id);

  RETURN_RESOURCE(t->rsrc_id);
}

//proto bool btp_timer_start(resource timer)
static PHP_FUNCTION(btp_timer_stop)
{
  zval *timer;
  btp_timer_t *t;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &timer) != SUCCESS) {
    RETURN_FALSE;
  }

  BTP_ZVAL_TO_TIMER(timer, t);

  //попытка остановить таймер, созданный когда btp был отключен
  if( t->rsrc_id == dummy_timer->rsrc_id ) {
    RETURN_TRUE;
  }

  if ( !t->started ) {
    php_error_docref(NULL TSRMLS_CC, E_NOTICE, "timer is already stopped");
    RETURN_FALSE;
  }

  btp_timer_stop(t TSRMLS_CC);
  btp_autoflush( TSRMLS_C );
  RETURN_TRUE;
}

//proto bool btp_timer_set_operation(resource timer, string operation)
static PHP_FUNCTION(btp_timer_set_operation)
{
  zval *timer;
  btp_timer_t *t;

  char *operation;
  int operation_len;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs", &timer, &operation, &operation_len) != SUCCESS) {
    RETURN_FALSE;
  }

  BTP_ZVAL_TO_TIMER(timer, t);

  //попытка остановить таймер, созданный когда btp был отключен
  if( t->rsrc_id == dummy_timer->rsrc_id ) {
    RETURN_TRUE;
  }

  if ( !t->started ) {
    php_error_docref(NULL TSRMLS_CC, E_NOTICE, "timer is already stopped");
    RETURN_FALSE;
  }

  efree(t->operation);
  t->operation = estrndup(operation, operation_len);
  t->operation_len = operation_len;

  RETURN_TRUE;
}

// proto bool btp_flush(stopped=true)
static PHP_FUNCTION( btp_flush )
{
  zend_bool stopped = 1;
  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|b", &stopped) != SUCCESS) {
    RETURN_FALSE;
  }

  if( !stopped ) {
    zend_hash_apply(&EG(regular_list), (apply_func_t)btp_timer_stop_helper TSRMLS_CC);
  }

  btp_flush_data(-1 TSRMLS_CC);
  zend_hash_apply(&EG(regular_list), (apply_func_t)btp_timer_release_helper TSRMLS_CC);

  RETURN_TRUE;
}

// proto btp_timer_count(service, server, operation, time = 0, server_id = 0)
static PHP_FUNCTION( btp_timer_count ) {

  if( !btp_enabled( TSRMLS_C ) ) {
    RETURN_TRUE;
  }

  btp_timer_t *t = NULL;
  char *service, *server, *operation;
  int service_len, server_len, operation_len;
  long time_value, server_id;
  unsigned int server_index, i;

  time_value = server_id = 0;
  if ( zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sss|ll", &service, &service_len, &server, &server_len, &operation, &operation_len, &time_value, &server_id) != SUCCESS ) {
    RETURN_FALSE;
  }

  server_index = -1;
  for(i = 0; i < BTP_G(servers_used); i++) {
    if( BTP_G(servers)[i].id == server_id ) {
      server_index = i;
      break;
    }
  }

  if( server_index == -1 ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp server id %ld is not found!", server_id);
    RETURN_FALSE;
  }

  t = btp_timer_ctor( 0 TSRMLS_CC);
  t->service = estrndup(service, service_len);
  t->service_len = service_len;
  t->server = estrndup(server, server_len);
  t->server_len = server_len;
  t->operation = estrndup(operation, operation_len);
  t->operation_len = operation_len;
  t->server_index = server_index;
  t->rsrc_id = zend_list_insert(t, le_btp_timer TSRMLS_CC);

  t->started = 0;
  if( time_value ) {
    t->value.tv_sec = time_value / BTP_MS_IN_SEC;
    t->value.tv_usec = time_value % BTP_MS_IN_SEC;
  }
  else {
    t->value.tv_usec = time_value;
  }
  BTP_G(servers)[i].stopped_count++;

  /* refcount++ so that the timer is shut down only on request finish if not stopped manually */
  //zend_list_addref( t->rsrc_id );

  btp_autoflush( TSRMLS_C );

  RETURN_TRUE;
}

// proto btp_timer_count_script(service, server, operation, script, time = 0, server_id = 0)
static PHP_FUNCTION( btp_timer_count_script ) {

  if( !btp_enabled( TSRMLS_C ) ) {
    RETURN_TRUE;
  }

  btp_timer_t *t = NULL;
  char *service, *server, *operation, *script;
  int service_len, server_len, operation_len, script_len;
  long time_value, server_id;
  unsigned int server_index, i;

  time_value = server_id = 0;
  if ( zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ssss|ll",
      &service, &service_len,
      &server, &server_len,
      &operation, &operation_len,
      &script, &script_len,
      &time_value, &server_id
    ) != SUCCESS
  ) {
    RETURN_FALSE;
  }

  server_index = -1;
  for(i = 0; i < BTP_G(servers_used); i++) {
    if( BTP_G(servers)[i].id == server_id ) {
      server_index = i;
      break;
    }
  }

  if( server_index == -1 ) {
    php_error_docref(NULL TSRMLS_CC, E_WARNING, "btp server id %ld is not found!", server_id);
    RETURN_FALSE;
  }

  t = btp_timer_ctor( 0 TSRMLS_CC);
  t->service = estrndup(service, service_len);
  t->service_len = service_len;
  t->server = estrndup(server, server_len);
  t->server_len = server_len;
  t->operation = estrndup(operation, operation_len);
  t->operation_len = operation_len;
  t->script = estrndup(script, script_len);
  t->script_len = script_len;
  t->server_index = server_index;
  t->rsrc_id = zend_list_insert(t, le_btp_timer TSRMLS_CC);

  t->started = 0;
  if( time_value ) {
    t->value.tv_sec = time_value / BTP_MS_IN_SEC;
    t->value.tv_usec = time_value % BTP_MS_IN_SEC;
  }
  else {
    t->value.tv_usec = time_value;
  }
  BTP_G(servers)[i].stopped_count++;

  /* refcount++ so that the timer is shut down only on request finish if not stopped manually */
  //zend_list_addref( t->rsrc_id );

  btp_autoflush( TSRMLS_C );

  RETURN_TRUE;
}
//--------------------------------PHP API-------------------------------------------

zend_function_entry btp_functions[] = {
  PHP_FE(btp_config_server_set, NULL)
  PHP_FE(btp_dump, NULL)
  PHP_FE(btp_dump_timer, NULL)
  PHP_FE(btp_script_name_set, NULL)
  PHP_FE(btp_timer_start, NULL)
  PHP_FE(btp_timer_stop, NULL)
  PHP_FE(btp_flush, NULL)
  PHP_FE(btp_timer_count, NULL)
  PHP_FE(btp_timer_count_script, NULL)
  PHP_FE(btp_timer_set_operation, NULL)
  {NULL, NULL, NULL}
};

static PHP_INI_MH(OnIniUpdate)
{
  if( strncmp( "btp.fpm_enable", entry->name, entry->name_length ) == 0 ) {
    zend_bool new_fpm_enable = atoi(new_value);
    if( !BTP_G(is_cli) && BTP_G(fpm_enable) && !new_fpm_enable ) {
      zend_hash_apply(&EG(regular_list), (apply_func_t)btp_timer_delete_helper TSRMLS_CC);
    }
    BTP_G(fpm_enable) = new_fpm_enable;
  }
  else if( strncmp( "btp.cli_enable", entry->name, entry->name_length ) == 0 ) {
    zend_bool new_cli_enable = atoi(new_value);
    if( BTP_G(is_cli) && BTP_G(cli_enable) && !new_cli_enable ) {
      zend_hash_apply(&EG(regular_list), (apply_func_t)btp_timer_delete_helper TSRMLS_CC);
    }
    BTP_G(cli_enable) = new_cli_enable;
  }
  else if( strncmp( "btp.autoflush_time", entry->name, entry->name_length ) == 0 ) {
    BTP_G(autoflush_time) = atoi(new_value);
  }
  else if( strncmp( "btp.autoflush_count", entry->name, entry->name_length ) == 0 ) {
    BTP_G(autoflush_count) = atoi(new_value);
  }

  return SUCCESS;
}

PHP_INI_BEGIN()
    PHP_INI_ENTRY("btp.cli_enable", "1", PHP_INI_ALL, OnIniUpdate)
    PHP_INI_ENTRY("btp.fpm_enable", "1", PHP_INI_ALL, OnIniUpdate)
    PHP_INI_ENTRY("btp.autoflush_time", "60", PHP_INI_ALL, OnIniUpdate)
    PHP_INI_ENTRY("btp.autoflush_count", "0", PHP_INI_ALL, OnIniUpdate)
PHP_INI_END()

static void php_btp_init_globals(zend_btp_globals *globals)
{
  memset(globals, 0, sizeof(*globals));
}

static PHP_MINIT_FUNCTION(btp)
{
  ZEND_INIT_MODULE_GLOBALS(btp, php_btp_init_globals, NULL);
  REGISTER_INI_ENTRIES();

  BTP_G(fpm_enable) = INI_BOOL("btp.fpm_enable");
  BTP_G(cli_enable) = INI_BOOL("btp.cli_enable");
  BTP_G(autoflush_time) = INI_INT("btp.autoflush_time");
  BTP_G(autoflush_count) = INI_INT("btp.autoflush_count");
  BTP_G(send_timer_start) = time(0);

  le_btp_timer = zend_register_list_destructors_ex(btp_timer_resource_dtor, NULL, BTP_RESOURCE_NAME, module_number);
  BTP_G(is_cli) = 0;
  if( sapi_module.name ) {
    char *part = estrndup(sapi_module.name, 3);
    BTP_G(is_cli) = (strcmp("cli", part) == 0);
    efree(part);
  }

  return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(btp)
{
  BTP_G(script_name_len) = 0;
  if( BTP_G(script_name) ) {
    efree(BTP_G(script_name));
    BTP_G(script_name) = 0;
  }

  while( BTP_G(servers_used) > 0 ) {
    btp_release_server(&BTP_G(servers)[BTP_G(servers_used) - 1]);
    BTP_G(servers_used)--;
  }

  return SUCCESS;
}

static PHP_RINIT_FUNCTION(btp)
{
  dummy_timer = btp_timer_ctor( 0 TSRMLS_CC);
  dummy_timer->rsrc_id = zend_list_insert(dummy_timer, le_btp_timer TSRMLS_CC);
  dummy_timer->started = 0;
  dummy_timer->deleted = BTP_DELETED_HARD;

  /* refcount++ чтобы не удалился */
  zend_list_addref( dummy_timer->rsrc_id );


  //инициализация глобальных счетчиков
  struct timeval t;
  btp_request_data_t *request_data = &BTP_G(request_data);
  if (gettimeofday(&t, 0) == 0) {
    timeval_cvt(&request_data->req_start, &t);
  } else {
    return FAILURE;
  }

  gethostname(request_data->hostname, sizeof(request_data->hostname) - 1);

  strcpy(request_data->hostgroup, "SCRIPT_");
  int index_dst = strlen("SCRIPT_");
  int index_src = 0;
  while( request_data->hostname[index_src] ) {
    if(
        request_data->hostname[index_src] < '0' ||
        request_data->hostname[index_src] > '9'
    ) {
      request_data->hostgroup[index_dst] = request_data->hostname[index_src];
      index_dst++;
    }
    index_src++;
  }
  request_data->hostgroup[index_dst] = '\0';

  BTP_G(in_rshutdown) = 0;

  return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(btp)
{
  //добавим глобальные счетчики
  if( BTP_G(script_name_len) ) {

    btp_request_data_t *request_data = &BTP_G(request_data);
    int i;

    for(i = 0; i < BTP_G(servers_used); i++) {
      //память
      btp_timer_t *global_timer = btp_timer_ctor( 0 TSRMLS_CC);
      global_timer->started = 0;
      global_timer->service = estrndup(request_data->hostgroup, strlen(request_data->hostgroup));
      global_timer->service_len = strlen(request_data->hostgroup);
      global_timer->server = estrndup(request_data->hostname, strlen(request_data->hostname));
      global_timer->server_len = strlen(request_data->hostname);
      global_timer->operation = estrndup("memory", strlen("memory"));
      global_timer->operation_len = strlen("memory");
      global_timer->value.tv_usec = zend_memory_peak_usage(1 TSRMLS_CC);
      global_timer->server_index = i;
      global_timer->rsrc_id = zend_list_insert(global_timer, le_btp_timer TSRMLS_CC);
      zend_list_addref( global_timer->rsrc_id );

      //время
      struct timeval req_finish;
      if( gettimeofday(&req_finish, 0) == 0 ) {
        global_timer = btp_timer_ctor( 0 TSRMLS_CC);
        global_timer->started = 0;
        global_timer->service = estrndup(request_data->hostgroup, strlen(request_data->hostgroup));
        global_timer->service_len = strlen(request_data->hostgroup);
        global_timer->server = estrndup(request_data->hostname, strlen(request_data->hostname));
        global_timer->server_len = strlen(request_data->hostname);
        global_timer->operation = estrndup("all", strlen("all"));
        global_timer->operation_len = strlen("all");
        timersub(&req_finish, &request_data->req_start, &global_timer->value);
        global_timer->server_index = i;
        global_timer->rsrc_id = zend_list_insert(global_timer, le_btp_timer TSRMLS_CC);
        zend_list_addref( global_timer->rsrc_id );
      }
    }
  }

#ifdef DEBUG2
  char *log_message = emalloc( 10000 );
  sprintf( log_message, "BTP: Request shutdown!!!\n");
  php_log_err( log_message TSRMLS_CC );
  efree(log_message);
#endif

  //застопим все таймеры и отправим все нах
  zend_hash_apply(&EG(regular_list), (apply_func_t)btp_timer_stop_helper TSRMLS_CC);
  btp_flush_data(-1 TSRMLS_CC);
  zend_hash_apply(&EG(regular_list), (apply_func_t)btp_timer_release_helper TSRMLS_CC);

  int i;

  BTP_G(script_name_len) = 0;
  if (BTP_G(script_name)) {
    efree(BTP_G(script_name));
    BTP_G(script_name) = 0;
  }

  for(i = 0; i < BTP_G(servers_used); i++) {
    btp_release_server(&BTP_G(servers)[i]);
  }
  BTP_G(servers_used) = 0;
  BTP_G(in_rshutdown) = 1;

  //decrement ref counter
  zend_list_delete( dummy_timer->rsrc_id );

  return SUCCESS;
}

static PHP_MINFO_FUNCTION(btp)
{
  php_info_print_table_start();
  php_info_print_table_header(2, "Btp support", "enabled");
  php_info_print_table_row(2, "Extension version", PHP_BTP_VERSION);
  php_info_print_table_end();

  DISPLAY_INI_ENTRIES();
}

zend_module_entry btp_module_entry = {
  STANDARD_MODULE_HEADER,
  "btp",
  btp_functions,
  PHP_MINIT(btp),
  PHP_MSHUTDOWN(btp),
  PHP_RINIT(btp),
  PHP_RSHUTDOWN(btp),
  PHP_MINFO(btp),
  PHP_BTP_VERSION,
  STANDARD_MODULE_PROPERTIES
};
