#include "common.h"
#include "php_network.h"
#include <sys/types.h>
#include <netinet/tcp.h>  /* TCP_NODELAY */
#include <sys/socket.h>

PHPAPI void redis_check_eof(RedisSock *redis_sock TSRMLS_DC)
{

    int eof = php_stream_eof(redis_sock->stream);
    while(eof) {
        redis_sock->stream = NULL;
        redis_sock_connect(redis_sock TSRMLS_CC);
	if(redis_sock->stream) {
            eof = php_stream_eof(redis_sock->stream);
	}
    }
}

PHPAPI zval *redis_sock_read_multibulk_reply_zval(INTERNAL_FUNCTION_PARAMETERS,
												  RedisSock *redis_sock TSRMLS_DC) {
    char inbuf[1024], *response;
    int response_len;

    redis_check_eof(redis_sock TSRMLS_CC);
    php_stream_gets(redis_sock->stream, inbuf, 1024);

    if(inbuf[0] != '*') {
        return NULL;
    }
    int numElems = atoi(inbuf+1);

    zval *z_tab;
    MAKE_STD_ZVAL(z_tab);
    array_init(z_tab);

    redis_sock_read_multibulk_reply_loop(INTERNAL_FUNCTION_PARAM_PASSTHRU,
                    redis_sock, z_tab, numElems);
	return z_tab;
}

/**
 * redis_sock_read_bulk_reply
 */
PHPAPI char *redis_sock_read_bulk_reply(RedisSock *redis_sock, int bytes)
{
    int offset = 0;
    size_t got;

    char * reply;

    redis_check_eof(redis_sock TSRMLS_CC);

    if (bytes == -1) {
        return NULL;
    } else {
        reply = emalloc(bytes+1);

        while(offset < bytes) {
            got = php_stream_read(redis_sock->stream, reply + offset, bytes-offset);
            offset += got;
        }
        char c;
        int i;
        for(i = 0; i < 2; i++) {
            php_stream_read(redis_sock->stream, &c, 1);
        }
    }

    reply[bytes] = 0;
    return reply;
}

/**
 * redis_sock_read
 */
PHPAPI char *redis_sock_read(RedisSock *redis_sock, int *buf_len TSRMLS_DC)
{

    char inbuf[1024];
    char *resp = NULL;

    redis_check_eof(redis_sock TSRMLS_CC);
    php_stream_gets(redis_sock->stream, inbuf, 1024);

    switch(inbuf[0]) {

        case '-':
            return NULL;

        case '+':
        case ':':
	    // Single Line Reply
            /* :123\r\n */
            *buf_len = strlen(inbuf) - 2;
            if(*buf_len >= 2) {
                resp = emalloc(1+*buf_len);
                memcpy(resp, inbuf, *buf_len);
                resp[*buf_len] = 0;
                return resp;
            } else {
                printf("protocol error \n");
                return NULL;
            }

        case '$':
            *buf_len = atoi(inbuf + 1);
            resp = redis_sock_read_bulk_reply(redis_sock, *buf_len);
            return resp;

        default:
            printf("protocol error, got '%c' as reply type byte\n", inbuf[0]);
    }

    return NULL;
}

void add_constant_long(zend_class_entry *ce, char *name, int value) {

    zval *constval;
    constval = pemalloc(sizeof(zval), 1);
    INIT_PZVAL(constval);
    ZVAL_LONG(constval, value);
    zend_hash_add(&ce->constants_table, name, 1 + strlen(name),
        (void*)&constval, sizeof(zval*), NULL);
}

int
integer_length(int i) {
    int sz = 0;
    int ci = abs(i);
    while (ci>0) {
            ci = (ci/10);
            sz += 1;
    }
    if(i == 0) { /* log 0 doesn't make sense. */
            sz = 1;
    } else if(i < 0) { /* allow for neg sign as well. */
            sz++;
    }
    return sz;
}

int
double_length(double d) {
        char *s;
        int ret = spprintf(&s, 0, "%F", d);
        efree(s);
        return ret;
}

/**
 * This command behave somehow like printf, except that strings need 2 arguments:
 * Their data and their size (strlen).
 * Supported formats are: %d, %i, %s
 */
//static  /!\ problem with static commands !!
int
redis_cmd_format(char **ret, char *format, ...) {

    char *p, *s;
    va_list ap;

    int total = 0, sz, ret_sz;
    int i, ci;
    unsigned int u;
    double dbl;
    char *double_str;
    int double_len;

    int stage; /* 0: count & alloc. 1: copy. */

    for(stage = 0; stage < 2; ++stage) {
        va_start(ap, format);
        total = 0;
        for(p = format; *p; ) {

            if(*p == '%') {
                switch(*(p+1)) {
                    case 's':
                        s = va_arg(ap, char*);
                        sz = va_arg(ap, int);
                        if(stage == 1) {
                            memcpy((*ret) + total, s, sz);
                        }
                        total += sz;
                        break;

                    case 'F':
                    case 'f':
                        /* use spprintf here */
                        dbl = va_arg(ap, double);
                        double_len = spprintf(&double_str, 0, "%F", dbl);
                        if(stage == 1) {
                            memcpy((*ret) + total, double_str, double_len);
                        }
                        total += double_len;
                        efree(double_str);
                        break;

                    case 'i':
                    case 'd':
                        i = va_arg(ap, int);
                        /* compute display size of integer value */
                        sz = 0;
                        ci = abs(i);
                        while (ci>0) {
                                ci = (ci/10);
                                sz += 1;
                        }
                        if(i == 0) { /* log 0 doesn't make sense. */
                                sz = 1;
                        } else if(i < 0) { /* allow for neg sign as well. */
                                sz++;
                        }
                        if(stage == 1) {
                            sprintf((*ret) + total, "%d", i);
                        }
                        total += sz;
                        break;
                }
                p++;
            } else {
                if(stage == 1) {
                    (*ret)[total] = *p;
                }
                total++;
            }

            p++;
        }
        if(stage == 0) {
            ret_sz = total;
            (*ret) = emalloc(ret_sz+1);
        } else {
            (*ret)[ret_sz] = 0;
            return ret_sz;
        }
    }
}

PHPAPI void redis_bulk_double_response(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval *z_tab TSRMLS_DC) {

    char *response;
    int response_len;

    zval *object = getThis();
    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    double ret = atof(response);
    efree(response);
    IF_MULTI_OR_PIPELINE() {
	add_next_index_double(z_tab, ret);
    } else {
    	RETURN_DOUBLE(ret);
    }
}

PHPAPI void redis_boolean_response(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval *z_tab TSRMLS_DC) {

    char *response;
    int response_len;
    char ret;

    zval *object = getThis();
    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
	IF_MULTI_OR_PIPELINE() {
            add_next_index_bool(z_tab, 0);
	    return;
	}
        RETURN_FALSE;
    }
    ret = response[0];
    efree(response);

    IF_MULTI_OR_PIPELINE() {
        if (ret == '+') {
            add_next_index_bool(z_tab, 1);
        } else {
            add_next_index_bool(z_tab, 0);
        }
    }

    if (ret == '+') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}

PHPAPI void redis_long_response(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval * z_tab TSRMLS_DC) {

    char *response;
    int response_len;
    zval *object = getThis();

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
	IF_MULTI_OR_PIPELINE() {
            add_next_index_bool(z_tab, 0);
	    return;
	}
        RETURN_FALSE;
    }

    if(response[0] == ':') {
        long ret = atol(response + 1);
        IF_MULTI_OR_PIPELINE() {
            add_next_index_long(z_tab, ret);
        }
        efree(response);
        RETURN_LONG(ret);
    } else {
        IF_MULTI_OR_PIPELINE() {
          add_next_index_null(z_tab);
        }
        efree(response);
        RETURN_FALSE;
    }
}

PHPAPI int redis_sock_read_multibulk_reply_zipped_with_flag(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval *z_tab, int flag TSRMLS_DC) {

	/*
	int ret = redis_sock_read_multibulk_reply(INTERNAL_FUNCTION_PARAM_PASSTHRU, redis_sock, z_tab TSRMLS_CC);
	array_zip_values_and_scores(return_value, 0);
	*/

    char inbuf[1024], *response;
    int response_len;

    redis_check_eof(redis_sock TSRMLS_CC);
    php_stream_gets(redis_sock->stream, inbuf, 1024);

    if(inbuf[0] != '*') {
        return -1;
    }
    int numElems = atoi(inbuf+1);
    zval *z_multi_result;
    MAKE_STD_ZVAL(z_multi_result);
    array_init(z_multi_result); /* pre-allocate array for multi's results. */

    redis_sock_read_multibulk_reply_loop(INTERNAL_FUNCTION_PARAM_PASSTHRU,
                    redis_sock, z_multi_result, numElems);

    array_zip_values_and_scores(z_multi_result, 0);

    zval *object = getThis();
    IF_MULTI_OR_PIPELINE() {
        add_next_index_zval(z_tab, z_multi_result);
    } else {
	    *return_value = *z_multi_result;
	    zval_copy_ctor(return_value);
	    efree(z_multi_result);
    }

	return 0;
}

PHPAPI int redis_sock_read_multibulk_reply_zipped(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval *z_tab TSRMLS_DC) {

	return redis_sock_read_multibulk_reply_zipped_with_flag(INTERNAL_FUNCTION_PARAM_PASSTHRU, redis_sock, z_tab, 1);
}

PHPAPI int redis_sock_read_multibulk_reply_zipped_strings(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval *z_tab TSRMLS_DC) {
	return redis_sock_read_multibulk_reply_zipped_with_flag(INTERNAL_FUNCTION_PARAM_PASSTHRU, redis_sock, z_tab, 0);
}

PHPAPI void redis_1_response(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval *z_tab TSRMLS_DC) {

	char *response;
	int response_len;
	char ret;

	zval *object = getThis();
	if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
		IF_MULTI_OR_PIPELINE() {
			add_next_index_bool(z_tab, 0);
			return;
		}
		RETURN_FALSE;
	}
	ret = response[1];
	efree(response);

	IF_MULTI_OR_PIPELINE() {
		if(ret == '1') {
			add_next_index_bool(z_tab, 1);
		} else {
			add_next_index_bool(z_tab, 0);
		}
	}

	if (ret == '1') {
		RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}

PHPAPI void redis_string_response(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock, zval *z_tab TSRMLS_DC) {

    char *response;
    int response_len;
    char ret;

    zval *object = getThis();

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        IF_MULTI_OR_PIPELINE() {
            add_next_index_bool(z_tab, 0);
	    return;
        }
        RETURN_FALSE;
    }
    IF_MULTI_OR_PIPELINE() {
        add_next_index_stringl(z_tab, response, response_len, 0);
    }

    RETURN_STRINGL(response, response_len, 0);
}


/**
 * redis_sock_create
 */
PHPAPI RedisSock* redis_sock_create(char *host, int host_len, unsigned short port,
                                                                       long timeout)
{
    RedisSock *redis_sock;

    redis_sock         = emalloc(sizeof *redis_sock);
    redis_sock->host   = emalloc(host_len + 1);
    redis_sock->stream = NULL;
    redis_sock->status = REDIS_SOCK_STATUS_DISCONNECTED;

    memcpy(redis_sock->host, host, host_len);
    redis_sock->host[host_len] = '\0';

    redis_sock->port    = port;
    redis_sock->timeout = timeout;

    return redis_sock;
}

/**
 * redis_sock_connect
 */
PHPAPI int redis_sock_connect(RedisSock *redis_sock TSRMLS_DC)
{
    struct timeval tv, *tv_ptr = NULL;
    char *host = NULL, *hash_key = NULL, *errstr = NULL;
    int host_len, err = 0;

    if (redis_sock->stream != NULL) {
        redis_sock_disconnect(redis_sock TSRMLS_CC);
    }

    tv.tv_sec  = redis_sock->timeout;
    tv.tv_usec = 0;

    host_len = spprintf(&host, 0, "%s:%d", redis_sock->host, redis_sock->port);

    if(tv.tv_sec != 0) {
        tv_ptr = &tv;
    }
    redis_sock->stream = php_stream_xport_create(host, host_len, ENFORCE_SAFE_MODE,
                                                 STREAM_XPORT_CLIENT
                                                 | STREAM_XPORT_CONNECT,
                                                 hash_key, tv_ptr, NULL, &errstr, &err
                                                );

    efree(host);

    /* set TCP_NODELAY */
    php_netstream_data_t *sock = (php_netstream_data_t*)redis_sock->stream->abstract;
    int tcp_flag = 1;
    int result = setsockopt(sock->socket, IPPROTO_TCP, TCP_NODELAY, (char *) &tcp_flag, sizeof(int));

    if (!redis_sock->stream) {
        efree(errstr);
        return -1;
    }

    php_stream_auto_cleanup(redis_sock->stream);

    if(tv.tv_sec != 0) {
        php_stream_set_option(redis_sock->stream, PHP_STREAM_OPTION_READ_TIMEOUT,
                              0, &tv);
    }
    php_stream_set_option(redis_sock->stream,
                          PHP_STREAM_OPTION_WRITE_BUFFER,
                          PHP_STREAM_BUFFER_NONE, NULL);

    redis_sock->status = REDIS_SOCK_STATUS_CONNECTED;

    return 0;
}

/**
 * redis_sock_server_open
 */
PHPAPI int redis_sock_server_open(RedisSock *redis_sock, int force_connect TSRMLS_DC)
{
    int res = -1;

    switch (redis_sock->status) {
        case REDIS_SOCK_STATUS_DISCONNECTED:
            return redis_sock_connect(redis_sock TSRMLS_CC);
        case REDIS_SOCK_STATUS_CONNECTED:
            res = 0;
        break;
        case REDIS_SOCK_STATUS_UNKNOWN:
            if (force_connect > 0 && redis_sock_connect(redis_sock TSRMLS_CC) < 0) {
                res = -1;
            } else {
                res = 0;

                redis_sock->status = REDIS_SOCK_STATUS_CONNECTED;
            }
        break;
    }

    return res;
}

/**
 * redis_sock_disconnect
 */
PHPAPI int redis_sock_disconnect(RedisSock *redis_sock TSRMLS_DC)
{
    int res = 0;

    if (redis_sock->stream != NULL) {
        redis_sock_write(redis_sock, "QUIT", sizeof("QUIT") - 1);

        redis_sock->status = REDIS_SOCK_STATUS_DISCONNECTED;
        php_stream_close(redis_sock->stream);
        redis_sock->stream = NULL;

        res = 1;
    }

    return res;
}

/**
 * redis_sock_read_multibulk_reply
 */
PHPAPI int redis_sock_read_multibulk_reply(INTERNAL_FUNCTION_PARAMETERS,
                                           RedisSock *redis_sock, zval *z_tab TSRMLS_DC)
{
    char inbuf[1024], *response;
    int response_len;

    redis_check_eof(redis_sock TSRMLS_CC);
    php_stream_gets(redis_sock->stream, inbuf, 1024);

    if(inbuf[0] != '*') {
        return -1;
    }
    int numElems = atoi(inbuf+1);
    zval *z_multi_result;
    MAKE_STD_ZVAL(z_multi_result);
    array_init(z_multi_result); /* pre-allocate array for multi's results. */

    redis_sock_read_multibulk_reply_loop(INTERNAL_FUNCTION_PARAM_PASSTHRU,
                    redis_sock, z_multi_result, numElems);

    zval *object = getThis();
    IF_MULTI_OR_PIPELINE() {
        add_next_index_zval(z_tab, z_multi_result);
    }

    *return_value = *z_multi_result;
    //zval_copy_ctor(return_value);
    // efree(z_multi_result);
    return 0;
}

PHPAPI int
redis_sock_read_multibulk_reply_loop(INTERNAL_FUNCTION_PARAMETERS, RedisSock *redis_sock,
                                     zval *z_tab, int numElems TSRMLS_DC)
{
    char *response;
    int response_len;

    while(numElems > 0) {
        int response_len;
        response = redis_sock_read(redis_sock, &response_len TSRMLS_CC);
        if(response != NULL) {
            add_next_index_stringl(z_tab, response, response_len, 0);
        } else {
            add_next_index_bool(z_tab, 0);
        }
        numElems --;
    }
    return 0;
}

/**
 * redis_sock_write
 */
PHPAPI int redis_sock_write(RedisSock *redis_sock, char *cmd, size_t sz)
{
    redis_check_eof(redis_sock TSRMLS_CC);
    return php_stream_write(redis_sock->stream, cmd, sz);
}

/**
 * redis_free_socket
 */
PHPAPI void redis_free_socket(RedisSock *redis_sock)
{
    efree(redis_sock->host);
    efree(redis_sock);
}
