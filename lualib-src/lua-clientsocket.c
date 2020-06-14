// simple lua socket library for client
// It's only for demo, limited feature. Don't use it in your project.
// Rewrite socket library by yourself .
#include "uv.h"
#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

#define CACHE_SIZE 0x1000	
uv_thread_t pid;

static int
lconnect(lua_State *L) {
	const char * addr = luaL_checkstring(L, 1);
	int port = luaL_checkinteger(L, 2);
	int fd = socket(AF_INET,SOCK_STREAM,0);
	struct sockaddr_in my_addr;

	my_addr.sin_addr.s_addr=inet_addr(addr);
	my_addr.sin_family=AF_INET;
	my_addr.sin_port=htons(port);

	int r = connect(fd,(struct sockaddr *)&my_addr,sizeof(struct sockaddr_in));

	if (r == -1) {
		return luaL_error(L, "Connect %s %d failed", addr, port);
	}

#ifdef _WINDOWS_
	unsigned long ul = 1;
	ioctlsocket(fd, FIONBIO, (unsigned long *)&ul);
#else
	int flag = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
#endif

	lua_pushinteger(L, fd);

	return 1;
}

static int
lclose(lua_State *L) {
	int fd = luaL_checkinteger(L, 1);
#if defined(WIN32) || defined(WIN64)
	closesocket(fd);
#else
	close(fd);
#endif
	return 0;
}

static void
block_send(lua_State *L, int fd, const char * buffer, int sz) {
	while(sz > 0) {
		int r = send(fd, buffer, sz, 0);
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			luaL_error(L, "socket error: %s", strerror(errno));
		}
		buffer += r;
		sz -= r;
	}
}

/*
	integer fd
	string message
 */
static int
lsend(lua_State *L) {
	size_t sz = 0;
	int fd = luaL_checkinteger(L,1);
	const char * msg = luaL_checklstring(L, 2, &sz);

	block_send(L, fd, msg, (int)sz);

	return 0;
}

/*
	intger fd
	string last
	table result

	return 
		boolean (true: data, false: block, nil: close)
		string last
 */

struct socket_buffer {
	void * buffer;
	int sz;
};

static int
lrecv(lua_State *L) {
	int fd = luaL_checkinteger(L,1);

	char buffer[CACHE_SIZE];
	int r = recv(fd, buffer, CACHE_SIZE, 0);
	if (r == 0) {
		lua_pushliteral(L, "");
		// close
		return 1;
	}
	if (r < 0) {
#ifdef _WINDOWS_
		if (WSAGetLastError() == WSAEWOULDBLOCK) {
			return 0;
		}
#else
		if (errno == EAGAIN || errno == EINTR) {
			return 0;
		}
#endif
		luaL_error(L, "socket error: %s", strerror(errno));
	}
	lua_pushlstring(L, buffer, r);
	return 1;
}

static int
lusleep(lua_State *L) {
	int n = luaL_checknumber(L, 1);
#ifdef _WINDOWS_
	n /= 1000;
	if (n < 20) n = 20;
	Sleep(n);
#else
	usleep(n);
#endif
	return 0;
}

// quick and dirty none block stdin readline

#define QUEUE_SIZE 1024

#ifdef _WINDOWS_
#define sn_atomic_test_set(p, set) InterlockedExchange(p, set)
#define sn_atomic_release(p) InterlockedExchange(p, 0)
#else
#define sn_atomic_test_set(p, set) __sync_lock_test_and_set(p, set)
#define sn_atomic_release(p) __sync_lock_release(p)
#endif // _WINDOWS_

#define LOCK(q) while (sn_atomic_test_set(&(q)->lock,1)) {}
#define UNLOCK(q) sn_atomic_release(&(q)->lock);

struct queue {
	int lock;
	int head;
	int tail;
	char * queue[QUEUE_SIZE];
};

static void
readline_stdin(void * arg) {
	struct queue * q = arg;
	char tmp[1024];
	while (!feof(stdin)) {
		if (fgets(tmp,sizeof(tmp),stdin) == NULL) {
			// read stdin failed
			exit(1);
		}
		int n = strlen(tmp) -1;

		char * str = malloc(n+1);
		memcpy(str, tmp, n);
		str[n] = 0;

		LOCK(q);
		q->queue[q->tail] = str;

		if (++q->tail >= QUEUE_SIZE) {
			q->tail = 0;
		}
		if (q->head == q->tail) {
			// queue overflow
			exit(1);
		}
		UNLOCK(q);
	}
	return;
}

static int
lreadstdin(lua_State *L) {
	struct queue *q = lua_touserdata(L, lua_upvalueindex(1));
	LOCK(q);
	if (q->head == q->tail) {
		UNLOCK(q);
		return 0;
	}
	char * str = q->queue[q->head];
	if (++q->head >= QUEUE_SIZE) {
		q->head = 0;
	}
	UNLOCK(q);
	lua_pushstring(L, str);
	free(str);
	return 1;
}

LUALIB_API int
luaopen_clientsocket(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "connect", lconnect },
		{ "recv", lrecv },
		{ "send", lsend },
		{ "close", lclose },
		{ "usleep", lusleep },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);

#ifdef _WINDOWS_
	WORD wVersionRequested;
	WSADATA wsaData;
	wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);
#endif

	struct queue * q = lua_newuserdata(L, sizeof(*q));
	memset(q, 0, sizeof(*q));
	lua_pushcclosure(L, lreadstdin, 1);
	lua_setfield(L, -2, "readstdin");

	uv_thread_create(&pid, readline_stdin, q);

	return 1;
}
