#include <sys/time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "hiredispool.h"
#include "logger.h"

#include "hiredis/hiredis.h"

#define MAX_REDIS_SOCKS 1000

static int redis_init_socketpool(REDIS_INSTANCE * inst);
static void redis_poolfree(REDIS_INSTANCE * inst);
static int connect_single_socket(REDIS_SOCKET *redisocket, REDIS_INSTANCE *inst);
static int redis_close_socket(REDIS_INSTANCE *inst, REDIS_SOCKET * redisocket);
static void save_last_error(REDIS_SOCKET *redisocket, redisContext *c);
static void set_last_error(REDIS_SOCKET *redisocket, const char *errstr);

int redis_pool_create(const REDIS_CONFIG* config, REDIS_INSTANCE** instance)
{
    int i;
    char* host;
    int port;
    REDIS_INSTANCE *inst;

    inst = (REDIS_INSTANCE *)malloc(sizeof(REDIS_INSTANCE));
    memset(inst, 0, sizeof(REDIS_INSTANCE));

    inst->config = (REDIS_CONFIG*)malloc(sizeof(REDIS_CONFIG));
    memset(inst->config, 0, sizeof(REDIS_CONFIG));

    if (config->endpoints == NULL || config->num_endpoints < 1) {
        logth(Error, "%s: Must provide 1 redis endpoint", __func__);
        redis_pool_destroy(inst);
        return -1;
    }

    /* Assign config */
    inst->config->endpoints = (REDIS_ENDPOINT*)malloc(sizeof(REDIS_ENDPOINT) * config->num_endpoints);
    memcpy(inst->config->endpoints, config->endpoints, sizeof(REDIS_ENDPOINT) * config->num_endpoints);
    inst->config->num_endpoints = config->num_endpoints;
    inst->config->connect_timeout = config->connect_timeout;
    inst->config->net_readwrite_timeout = config->net_readwrite_timeout;
    inst->config->num_redis_socks = config->num_redis_socks;
    inst->config->connect_failure_retry_delay = config->connect_failure_retry_delay;

    /* Check config */
    if (inst->config->num_redis_socks > MAX_REDIS_SOCKS) {
        logth(Error, "%s: "
               "Number of redis sockets (%d) cannot exceed MAX_REDIS_SOCKS (%d)",
               __func__,
               inst->config->num_redis_socks, MAX_REDIS_SOCKS);
        redis_pool_destroy(inst);
        return -1;
    }

    if (inst->config->connect_timeout <= 0)
        inst->config->connect_timeout = 0;
    if (inst->config->net_readwrite_timeout <= 0)
        inst->config->net_readwrite_timeout = 0;
    if (inst->config->connect_failure_retry_delay <= 0)
        inst->config->connect_failure_retry_delay = -1;

    for (i = 0; i < inst->config->num_endpoints; i++) {
        host = inst->config->endpoints[i].host;
        port = inst->config->endpoints[i].port;
        if (host == NULL || strlen(host) == 0 || port <= 0 || port > 65535) {
            logth(Error, "%s: Invalid redis endpoint @%d: %s:%d", __func__, i, host, port);
            redis_pool_destroy(inst);
            return -1;
        }
        logth(Info, "%s: Got redis endpoint @%d: %s:%d", __func__, i, host, port);
    }

    logth(Info, "%s: Attempting to connect to above endpoints "
        "with connect_timeout %d net_readwrite_timeout %d",
        __func__,
        inst->config->connect_timeout, inst->config->net_readwrite_timeout);

    if (redis_init_socketpool(inst) < 0) {
        redis_pool_destroy(inst);
        return -1;
    }

    *instance = inst;

    return 0;
}

int redis_pool_destroy(REDIS_INSTANCE* instance)
{
    REDIS_INSTANCE *inst = instance;

    if (inst == NULL)
        return -1;

    if (inst->redis_pool) {
        redis_poolfree(inst);
    }

    if (inst->config) {
        /*
         *  Free up dynamically allocated pointers.
         */
        free(inst->config->endpoints);

        free(inst->config);
        inst->config = NULL;
    }

    free(inst);

    return 0;
}

static int redis_init_socketpool(REDIS_INSTANCE * inst)
{
    int i, rcode;
    /*int success = 0;*/
    REDIS_SOCKET *redisocket;

    inst->connect_after = 0;
    inst->redis_pool = NULL;

    for (i = 0; i < inst->config->num_redis_socks; i++) {
        /*logth(Debug, "%s: starting %d", __func__, i);*/

        redisocket = (REDIS_SOCKET *)malloc(sizeof(REDIS_SOCKET));
        redisocket->conn = NULL;
        redisocket->id = i;
        redisocket->backup = i % inst->config->num_endpoints;
        redisocket->state = redis_socket::sockunconnected;
        redisocket->inuse = 0;

        rcode = pthread_mutex_init(&redisocket->mutex, NULL);
        if (rcode != 0) {
            logth(Error, "%s: "
                   "Failed to init lock: returns (%d)", __func__, rcode);
            free(redisocket);
            return -1;
        }
        #if 0  /* We disable connecting the sockets here to minimize the init time */
        if (time(NULL) > inst->connect_after) {
            /*
             *  This sets the redisocket->state, and
             *  possibly also inst->connect_after
             */
            if (connect_single_socket(redisocket, inst) == 0) {
                success = 1;
            }
        }
        #endif
        /* Add this socket to the list of sockets */
        redisocket->next = inst->redis_pool;
        inst->redis_pool = redisocket;
    }

    inst->last_used = NULL;
    #if 0
    if (!success) {
        logth(Error, "%s: Failed to connect to any redis server.", __func__);
    }
    #endif
    return 0;
}

static void redis_poolfree(REDIS_INSTANCE * inst)
{
    REDIS_SOCKET *cur;
    REDIS_SOCKET *next;

    for (cur = inst->redis_pool; cur; cur = next) {
        next = cur->next;
        redis_close_socket(inst, cur);
    }

    inst->redis_pool = NULL;
    inst->last_used = NULL;
}

/*
 * Connect to a server.  If error, set this socket's state to be
 * "sockunconnected" and set a grace period, during which we won't try
 * connecting again (to prevent unduly lagging the server and being
 * impolite to a server that may be having other issues).  If
 * successful in connecting, set state to sockconnected.
 * - hh
 */
static int connect_single_socket(REDIS_SOCKET *redisocket, REDIS_INSTANCE *inst)
{
    int i;
    redisContext* c;
    struct timeval timeout[2];
    char *host;
    int port;

    logth(Info, "%s: Attempting to connect #%d @%d",
           __func__, redisocket->id, redisocket->backup);

    /* convert timeout (ms) to timeval */
    timeout[0].tv_sec = inst->config->connect_timeout / 1000;
    timeout[0].tv_usec = 1000 * (inst->config->connect_timeout % 1000);
    timeout[1].tv_sec = inst->config->net_readwrite_timeout / 1000;
    timeout[1].tv_usec = 1000 * (inst->config->net_readwrite_timeout % 1000);

    for (i = 0; i < inst->config->num_endpoints; i++) {
        /*
         * Get the target host and port from the backup index
         */
        host = inst->config->endpoints[redisocket->backup].host;
        port = inst->config->endpoints[redisocket->backup].port;

        c = redisConnectWithTimeout(host, port, timeout[0]);
        if (c && c->err == 0) {
            logth(Info, "%s: Connected new redis handle #%d @%d",
                __func__, redisocket->id, redisocket->backup);
            redisocket->conn = c;
            redisocket->state = redis_socket::sockconnected;
            if (inst->config->num_endpoints > 1) {
                /* Select the next _random_ endpoint as the new backup */
                redisocket->backup = (redisocket->backup + (1 +
                        rand() % (inst->config->num_endpoints - 1)
                    )) % inst->config->num_endpoints;
            }

            if (redisSetTimeout(c, timeout[1]) != REDIS_OK) {
                logth(Error, "%s: Failed to set timeout: blocking-mode: %d, %s",
                    __func__, (c->flags & REDIS_BLOCK), c->errstr);
            }

            if (redisEnableKeepAlive(c) != REDIS_OK) {
                logth(Error, "%s: Failed to enable keepalive: %s",
                    __func__, c->errstr);
            }

            return 0;
        }

        /* We have tried the last one but still fail */
        if (i == inst->config->num_endpoints - 1)
            break;

        /* We have more backups to try */
        if (c) {
            logth(Warn, "%s: Failed to connect redis handle #%d @%d: %s, trying backup",
                __func__, redisocket->id, redisocket->backup, c->errstr);
            redisFree(c);
        } else {
            logth(Warn, "%s: can't allocate redis handle #%d @%d, trying backup",
                __func__, redisocket->id, redisocket->backup);
        }
        redisocket->backup = (redisocket->backup + 1) % inst->config->num_endpoints;
    }

    /*
     *  Error, or SERVER_DOWN.
     */
    if (c) {
        logth(Error, "%s: Failed to connect redis handle #%d @%d: %s",
            __func__, redisocket->id, redisocket->backup, c->errstr);
        /* Keep the err and reason temporarily in the sock */
        save_last_error(redisocket, c);
        redisFree(c);
    } else {
        logth(Error, "%s: can't allocate redis handle #%d @%d",
            __func__, redisocket->id, redisocket->backup);
        set_last_error(redisocket, "Not enough memory");
    }
    redisocket->conn = NULL;
    redisocket->state = redis_socket::sockunconnected;
    redisocket->backup = (redisocket->backup + 1) % inst->config->num_endpoints;

    inst->connect_after = time(NULL) + inst->config->connect_failure_retry_delay;

    return -1;
}

static void save_last_error(REDIS_SOCKET *redisocket, redisContext *c)
{
    redisocket->err = c->err;
    memcpy(redisocket->errstr, c->errstr, sizeof(c->errstr));
    redisocket->errport = c->tcp.port;

    strncpy(redisocket->errhost, c->tcp.host, sizeof(redisocket->errhost));
    redisocket->errhost[sizeof(redisocket->errhost) - 1] = '\0';
}

static void set_last_error(REDIS_SOCKET *redisocket, const char *errstr)
{
    redisocket->err = -1;
    redisocket->errport = -1;
    strcpy(redisocket->errhost, "N/A");

    strncpy(redisocket->errstr, errstr, sizeof(redisocket->errstr));
    redisocket->errstr[sizeof(redisocket->errstr) - 1] = '\0';
}

static int redis_close_socket(REDIS_INSTANCE *inst, REDIS_SOCKET * redisocket)
{
    int rcode;

    (void)inst;

    logth(Info, "%s: Closing redis socket =%d #%d @%d", __func__,
           redisocket->state, redisocket->id, redisocket->backup);

    if (redisocket->state == redis_socket::sockconnected) {
        redisFree((redisContext*)redisocket->conn);
    }

    if (redisocket->inuse) {
        logth(Warn, "%s: I'm still in use. Bug?", __func__);
    }

    rcode = pthread_mutex_destroy(&redisocket->mutex);
    if(rcode != 0) {
        logth(Warn, "%s: Failed to destroy lock: returns (%d)",
            __func__, rcode);
    }

    free(redisocket);
    return 0;
}

REDIS_SOCKET * redis_get_socket(REDIS_INSTANCE * inst)
{
    REDIS_SOCKET *cur, *start;
    int tried_to_connect = 0;
    int unconnected = 0;
    int rcode, locked;

    /*
     *  Start at the last place we left off.
     */
    start = inst->last_used;
    if (!start) start = inst->redis_pool;

    cur = start;

    locked = 0;
    while (cur) {
        /*
         *  If this socket is in use by another thread,
         *  skip it, and try another socket.
         *
         *  If it isn't used, then grab it ourselves.
         */
        if ((rcode = pthread_mutex_trylock(&cur->mutex)) != 0) {
            goto next;
        } /* else we now have the lock */
        else {
            locked = 1;
            /*logth(Debug, "%s: Obtained lock with handle %d", __func__, cur->id);*/
        }

        if(cur->inuse == 1) {
            if (locked) {
                if((rcode = pthread_mutex_unlock(&cur->mutex)) != 0) {
                    logth(Fatal, "%s: "
                        "Can not release lock with handle %d: returns (%d)",
                        __func__, cur->id, rcode);
                } else {
                    /*logth(Debug, "%s: Released lock with handle %d", __func__, cur->id);*/
                }
            }
            goto next;
        }
        else {
            cur->inuse = 1;
        }

         /*
         *  If we happen upon an unconnected socket, and
         *  this instance's grace period on
         *  (re)connecting has expired, then try to
         *  connect it.  This should be really rare.
         */
        if ((cur->state == redis_socket::sockunconnected) && (time(NULL) > inst->connect_after)) {
            logth(Info, "%s: "
                   "Trying to (re)connect unconnected handle %d ...",
                   __func__, cur->id);
            tried_to_connect++;
            connect_single_socket(cur, inst);
        }

        /* if we still aren't connected, ignore this handle */
        if (cur->state == redis_socket::sockunconnected) {
            logth(Info, "%s: "
                   "Ignoring unconnected handle %d ...",
                   __func__, cur->id);
            unconnected++;

            cur->inuse = 0;

            if((rcode = pthread_mutex_unlock(&cur->mutex)) != 0) {
                logth(Fatal, "%s: "
                       "Can not release lock with handle %d: returns (%d)",
                       __func__, cur->id, rcode);
            } else {
                /*logth(Debug, "%s: Released lock with handle %d", __func__, cur->id);*/
            }

            goto next;
        }

        /* should be connected, grab it */
        logth(Debug, "%s: Obtained redis socket id: %d",
               __func__, cur->id);

        if (unconnected != 0 || tried_to_connect != 0) {
            logth(Info, "%s: "
                   "got socket %d after skipping %d unconnected handles, "
                   "tried to reconnect %d though",
                   __func__, cur->id, unconnected, tried_to_connect);
        }

        /*
         *  The socket is returned in the locked
         *  state.
         *
         *  We also remember where we left off,
         *  so that the next search can start from
         *  here.
         *
         *  Note that multiple threads MAY over-write
         *  the 'inst->last_used' variable.  This is OK,
         *  as it's a pointer only used for reading.
         */
        inst->last_used = cur->next;
        return cur;

        /* move along the list */
    next:
        cur = cur->next;

        /*
         *  Because we didnt start at the start, once we
         *  hit the end of the linklist, we should go
         *  back to the beginning and work toward the
         *  middle!
         */
        if (!cur) {
            cur = inst->redis_pool;
        }

        /*
         *  If we're at the socket we started
         */
        if (cur == start) {
            break;
        }
    }

    /* We get here if every redis handle is unconnected and
     * unconnectABLE, or in use */
    logth(Error, "%s: "
           "There are no redis handles to use! skipped %d, tried to connect %d",
           __func__, unconnected, tried_to_connect);
    return NULL;
}

int redis_release_socket(REDIS_INSTANCE * inst, REDIS_SOCKET * redisocket)
{
    int rcode;

    (void)inst;
    if (redisocket == NULL) {
        return 0;
    }

    if (redisocket->inuse != 1) {
        logth(Fatal, "%s: I'm NOT in use. Bug?", __func__);
    }
    redisocket->inuse = 0;

    if((rcode = pthread_mutex_unlock(&redisocket->mutex)) != 0) {
        logth(Fatal, "%s: "
               "Can not release lock with handle %d: returns (%d)",
               __func__, redisocket->id, rcode);
    } else {
        /*logth(Debug, "%s: Released lock with handle %d", __func__, redisocket->id);*/
    }

    logth(Debug, "%s: Released redis socket id: %d", __func__, redisocket->id);

    return 0;
}

void* redis_command(REDIS_SOCKET* redisocket, REDIS_INSTANCE* inst, const char* format, ...)
{
    va_list ap;
    void *reply;
    va_start(ap, format);
    reply = redis_vcommand(redisocket, inst, format, ap);
    va_end(ap);
    return reply;
}

void* redis_vcommand(REDIS_SOCKET* redisocket, REDIS_INSTANCE* inst, const char* format, va_list ap)
{
    va_list ap2;
    void *reply;
    redisContext* c;

    va_copy(ap2, ap);

    /* forward to hiredis API */
    c = (redisContext*)redisocket->conn;
    reply = redisvCommand(c, format, ap);

    if (reply == NULL) {
        /* Once an error is returned the context cannot be reused and you shoud
           set up a new connection.
         */
        logth(Warn, "%s: Failed to execute redis command #%d @%d: %s, trying again",
            __func__, redisocket->id, redisocket->backup, c->errstr);

        /* close the socket that failed */
        redisFree(c);

        /* reconnect the socket */
        if (connect_single_socket(redisocket, inst) < 0) {
            logth(Error, "%s: Reconnect failed, server down?", __func__);
            goto quit;
        }

        /* retry on the newly connected socket */
        c = (redisContext*)redisocket->conn;
        reply = redisvCommand(c, format, ap2);

        if (reply == NULL) {
            logth(Error, "%s: Failed after reconnect: %s (%d)", __func__, c->errstr, c->err);

            /* Keep the err and reason temporarily in the sock */
            save_last_error(redisocket, c);

            /* do not need clean up here because the next caller will retry. */
            goto quit;
        }
    }

quit:
    va_end(ap2);
    return reply;
}

void* redis_command_argv(REDIS_SOCKET* redisocket, REDIS_INSTANCE* inst, int argc, const char **argv, const size_t *argvlen)
{
    void *reply;
    redisContext* c;

    /* forward to hiredis API */
    c = (redisContext*)redisocket->conn;
    reply = redisCommandArgv(c, argc, argv, argvlen);

    if (reply == NULL) {
        /* Once an error is returned the context cannot be reused and you shoud
        set up a new connection.
        */
        logth(Warn, "%s: Failed to execute redis command #%d @%d: %s, trying again",
            __func__, redisocket->id, redisocket->backup, c->errstr);

        /* close the socket that failed */
        redisFree(c);

        /* reconnect the socket */
        if (connect_single_socket(redisocket, inst) < 0) {
            logth(Error, "%s: Reconnect failed, server down?", __func__);
            goto quit;
        }

        /* retry on the newly connected socket */
        c = (redisContext*)redisocket->conn;
        reply = redisCommandArgv(c, argc, argv, argvlen);

        if (reply == NULL) {
            logth(Error, "%s: Failed after reconnect: %s (%d)", __func__, c->errstr, c->err);

            /* Keep the err and reason temporarily in the sock */
            save_last_error(redisocket, c);

            /* do not need clean up here because the next caller will retry. */
            goto quit;
        }
    }

quit:
    return reply;
}
