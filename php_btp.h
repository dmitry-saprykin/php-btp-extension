#ifndef PHP_BTP_H
#define PHP_BTP_H

#include <netinet/in.h>

extern zend_module_entry btp_module_entry;
#define phpext_btp_ptr &btp_module_entry

#ifdef ZTS
#include "TSRM.h"
#endif

#define PHP_BTP_VERSION "0.0.1"

#define SERVER_HOST_MAXLEN 100
#define SERVER_PORT_MAXLEN 10
#define SERVER_LIST_MAXLEN 10

typedef struct _btp_request_data_t {
  char hostname[ 128 ];
  char hostgroup[ 150 ];
  struct timeval req_start;
} btp_request_data_t;

typedef struct _btp_server_t {
  int socket;
  char host[ SERVER_HOST_MAXLEN + 1 ];
  char port[ SERVER_PORT_MAXLEN + 1 ];
  unsigned short id;
  unsigned short stopped_count;
  struct sockaddr_storage sockaddr;
  size_t sockaddr_len;
} btp_server_t;

ZEND_BEGIN_MODULE_GLOBALS(btp)
  btp_server_t servers[ SERVER_LIST_MAXLEN ];
  unsigned short servers_used;
  char *script_name;
  int script_name_len;
  zend_bool in_rshutdown;
  zend_bool is_cli;
  zend_bool cli_enable;
  zend_bool fpm_enable;
  time_t send_timer_start;
  unsigned int autoflush_time;
  unsigned int autoflush_count;
  btp_request_data_t request_data;
ZEND_END_MODULE_GLOBALS(btp)

#ifdef ZTS
#define BTP_G(v) TSRMG(btp_globals_id, zend_btp_globals *, v)
#else
#define BTP_G(v) (btp_globals.v)
#endif

#endif	/* PHP_BTP_H */
