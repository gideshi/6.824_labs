// RPC stubs for clients to talk to lock_server, and cache the locks
// see lock_client.cache.h for protocol details.

#include "lock_client_cache.h"
#include "rpc.h"
#include <sstream>
#include <iostream>
#include <stdio.h>
#include "tprintf.h"
#include "zdebug.h"

using namespace std;

int lock_client_cache::last_port = 0;

lock_client_cache::lock_client_cache(std::string xdst, 
				     class lock_release_user *_lu)
  : lock_client(xdst), lu(_lu)
{
  srand(time(NULL)^last_port);
  rlock_port = ((rand()%32000) | (0x1 << 10));
  const char *hname;
  // VERIFY(gethostname(hname, 100) == 0);
  hname = "127.0.0.1";
  std::ostringstream host;
  host << hname << ":" << rlock_port;
  id = host.str();
  last_port = rlock_port;
  rpcs *rlsrpc = new rpcs(rlock_port);
  rlsrpc->reg(rlock_protocol::revoke, this, &lock_client_cache::revoke_handler);
  rlsrpc->reg(rlock_protocol::retry, this, &lock_client_cache::retry_handler);
  pthread_mutex_init(&mutex, NULL);
}

/*
          acquire
  NONE -------------> ACQUIRING ------------> LOCKED
  FREE -------------> LOCKED
  LOCKED -----------> wait wait_cond ------------> loop
  ACQUIRING --------> wait wait_cond ------------> loop
  RELEASING --------> wait release_cond ---------> loop
            
          release
  if revoke > 0:
      release to server, LOCKED -> NONE, signal wait_cond and release_cond
  else:
      NONE -------------> ERROR
      FREE -------------> ERROR
      LOCKED -----------> FREE and signal wait_cond
      RELEASING --------> release to server, to NONE, signal wait_cond and release_cond
      ACQUIRING --------> ERROR

            revoke
  LOCKED -----------> RELEASING
  FREE   -----------> RELEASING ------------> release to server, to NONE, signal wait_cond and release_cond 
  NONE   -----------> revoke++
  RELEASING --------> revoke++
  ACQUIRING --------> revoke++

note:
1. we must use the lock at least once when received revoke before using the lock, or we will encounter deadlock in such situation:
    a lot of clients wants to get the lock, and as soon as a client get the lock, another client send him a revoke before he could use, thus nobody could finish the task

2. If a client closed connection and failed to release the lock, then nobody else could get the lock because revoke won't get the lock back.
        
*/

lock_protocol::status
lock_client_cache::acquire(lock_protocol::lockid_t lid)
{
    map<lock_protocol::lockid_t, ClientCacheLock>::iterator it;
    bool loop = false, acquire_flag = false;
    {
        ScopedLock _(&mutex);
        if (!locks.count(lid)) {        // if not exist
           ClientCacheLock &ccl = locks[lid];
           ccl.state = ClientCacheLock::NONE;
           pthread_cond_init(&ccl.wait_cond, NULL);
           pthread_cond_init(&ccl.retry_cond, NULL);
           pthread_cond_init(&ccl.release_cond, NULL);
           ccl.owner = -1;
           ccl.revoke = 0;
           ccl.retry = 0;
        };
        it = locks.find(lid);
        do {
            switch(it->second.state) {
                case ClientCacheLock::NONE:
                    it->second.state = ClientCacheLock::ACQUIRING;
                    Z("%s lock %llx NONE -> ACQUIRING", id.c_str(), lid);
                    acquire_flag = true;
                    loop = false;
                    break;

                case ClientCacheLock::FREE:
                    loop = false;
                    it->second.state = ClientCacheLock::LOCKED;
                    Z("%s lock %llx FREE -> LOCKED", id.c_str(), lid);
                    it->second.owner = pthread_self();
                    Z("%s::%lu got lock %llx from FREE state", id.c_str(), pthread_self(), lid);
                    break;

                case ClientCacheLock::ACQUIRING:
                case ClientCacheLock::LOCKED:
                    while (it->second.state == ClientCacheLock::ACQUIRING ||
                           it->second.state == ClientCacheLock::LOCKED) {
                        pthread_cond_wait(&it->second.wait_cond, &mutex);
                    };
                    loop = true;
                    break;

                case ClientCacheLock::RELEASING:
                    Z("%s wait for release_cond, lock %llx", id.c_str(), lid);
                    while (it->second.state == ClientCacheLock::RELEASING) {
                        pthread_cond_wait(&it->second.release_cond, &mutex);
                    }
                    loop = true;
                    break;

                default:
                    loop = false;
                    break;
            }
        } while (loop);
    }       // scoped lock end
    if (acquire_flag) {
        int ret, r;
        while (true) {
            Z("%s calling acquire RPC for lock %llx", id.c_str(), lid);
            ret = cl->call(lock_protocol::acquire, lid, id, r);
            Z("%s acquire RPC for lock %llx, ret is %s !!!!!!!!!!!!!!!!!!!!!", id.c_str(), lid, ret == lock_protocol::OK ? "OK":"RETRY");
            {
                ScopedLock __(&mutex);
                if (ret == lock_protocol::OK) {
                    it->second.state = ClientCacheLock::LOCKED;
                    Z("%s lock %llx ACQUIRING -> LOCKED", id.c_str(), lid);
                    it->second.owner = pthread_self();
                    break;
                }
                Z("%s waiting for retry_cond of lock %llx", id.c_str(), lid);
                while (!it->second.retry) {
                    pthread_cond_wait(&it->second.retry_cond, &mutex);
                }
                it->second.retry = 0;
            }
        }
    }
    return lock_protocol::OK;
}

lock_protocol::status
lock_client_cache::release(lock_protocol::lockid_t lid)
{
    bool release_flag = false;
    map<lock_protocol::lockid_t, ClientCacheLock>::iterator it;
    {
        ScopedLock _(&mutex);
        it = locks.find(lid);
        if (it == locks.end()) {
            ERR("hey, lock not found");
            return lock_protocol::RETRY;
        }
        if (it->second.revoke) {
            it->second.revoke--;
            release_flag = true;
        } else {
            switch (it->second.state) {
                case ClientCacheLock::NONE:
                    ERR("hey, you do not have the lock");
                    break;

                case ClientCacheLock::FREE:
                    ERR("releasing a free lock?");
                    break;

                case ClientCacheLock::LOCKED:
                    it->second.state = ClientCacheLock::FREE;
                    Z("%s::%lu, lock %llx, in release: LOCKED -> FREE, about to signal wait_cond", id.c_str(), pthread_self(), lid);
                    pthread_cond_signal(&it->second.wait_cond);
                    break;

                case ClientCacheLock::ACQUIRING:
                    ERR("relesing non-free locks");
                    break;

                case ClientCacheLock::RELEASING:
                    release_flag = true;
                    break;

                default:
                    break;
            }
        }
    }       // scoped lock end
    if (release_flag) {
        if (lu) {
            Z("do release");
            lu->dorelease(lid);
        } else {
            ERR("lu is null");
        }
        Z("%s, lock %llx, releasing lock to server", id.c_str(), lid);
        int r;
        int ret = cl->call(lock_protocol::release, lid, id, r);
        if (ret != lock_protocol::OK) {
            ERR("release failed");
        };
        {
            ScopedLock _(&mutex);
            it->second.state = ClientCacheLock::NONE;
            pthread_cond_broadcast(&it->second.wait_cond);
            pthread_cond_broadcast(&it->second.release_cond);
            Z("%s, lock %llx, state -> NONE, about to broadcast release_cond", id.c_str(), lid);
        }
    }
    return lock_protocol::OK;
}

rlock_protocol::status
lock_client_cache::revoke_handler(lock_protocol::lockid_t lid, 
                                  int &)
{
    map<lock_protocol::lockid_t, ClientCacheLock>::iterator it;
    bool release_flag = false;
    {
        ScopedLock _(&mutex);
        it = locks.find(lid);
        if (it == locks.end()) {
            ERR("lock not found");
            return lock_protocol::RETRY;
        }
        switch(it->second.state) {
            case ClientCacheLock::NONE:
                it->second.revoke++;
                Z("%s, revoke lock %llx in NONE state, revoke++, is %d", id.c_str(), lid, it->second.revoke);
                break;

            case ClientCacheLock::FREE:
                Z("%s, revoke lock %llx in FREE state", id.c_str(), lid);
                it->second.state = ClientCacheLock::RELEASING;
                Z("%s, revoke lock %llx, state -> RELEASING", id.c_str(), lid);
                release_flag = true;
                break;

            case ClientCacheLock::LOCKED:
                it->second.state = ClientCacheLock::RELEASING;
                Z("%s, revoke lock %llx, LOCKED -> RELEASING", id.c_str(), lid);
                break;

            case ClientCacheLock::ACQUIRING:
                it->second.revoke++;
                Z("%s, revoke lock %llx in ACQUIRING state, revoke++, is %d", id.c_str(), lid, it->second.revoke);
                break;

            case ClientCacheLock::RELEASING:
                it->second.revoke++;
                Z("%s, revoke lock %llx in RELEASING state, revoke++, is %d", id.c_str(), lid, it->second.revoke);
                break;

            default:
                ERR("state non %d ", it->second.state);
                break;
        }
    }       // scoped lock end
    if (release_flag) {
        if (lu) {
            Z("do release");
            lu->dorelease(lid);
        } else {
            ERR("lu is null");
        }
        int r;
        Z("%s, calling RPC release for lock %llx", id.c_str(), lid);
        int ret = cl->call(lock_protocol::release, lid, id, r);
        Z("%s, RPC release for lock %llx returned %d", id.c_str(), lid, ret);
        if (ret != lock_protocol::OK) {
            ERR("release failed");
        };
        {
            ScopedLock _(&mutex);
            it->second.state = ClientCacheLock::NONE;
            pthread_cond_broadcast(&it->second.wait_cond);  // must signal wait_cond, or those threads waiting for wait_cond won't be signaled it no more threads come
            pthread_cond_broadcast(&it->second.release_cond);
            Z("%s, lock %llx -> NONE", id.c_str(), lid);
        }
    }
    return lock_protocol::OK;
}

rlock_protocol::status
lock_client_cache::retry_handler(lock_protocol::lockid_t lid, 
                                 int &)
{
    map<lock_protocol::lockid_t, ClientCacheLock>::iterator it;
    it = locks.find(lid);
    if (it == locks.end()) {
        ERR("retry an empty lock");
    } else {
        {
            ScopedLock _(&mutex);   // must lock, or retry may be reassigned outside after signal
            Z("%s signal retry_cond for lock %llx", id.c_str(), lid);
            pthread_cond_signal(&it->second.retry_cond);
            it->second.retry = 1;
        }
    }
    return lock_protocol::OK;
}



