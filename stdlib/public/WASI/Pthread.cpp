// SPDX-License-Identifier: 0BSD
// prototypes taken from opengroup
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

#define STUB() do {fprintf(stderr, "FakePthread: unsupported %s\n", __func__);abort();}while(0)

// mutexes: just no-ops

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
	return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
	return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
	return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
	return 0;
}

// pthread_cond: STUB

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
	return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
	STUB();
}

int pthread_cond_timedwait(pthread_cond_t *cond, 
    pthread_mutex_t *mutex, const struct timespec *abstime) {
	STUB();
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
	return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
	return 0;
}

// tls

int pthread_key_create(pthread_key_t *key, void (*destructor)(void*)) {
	STUB();
}

void *pthread_getspecific(pthread_key_t key) {
	STUB();
}

int pthread_setspecific(pthread_key_t key, const void *value) {
	STUB();
}

// threads

pthread_t pthread_self() {
	return (pthread_t)1234;
}

#undef pthread_equal

int pthread_equal(pthread_t t1, pthread_t t2) {
	return t1 == t2;
}

int pthread_join(pthread_t thread, void **value_ptr) {
	STUB();
}

int pthread_detach(pthread_t thread) {
	STUB();
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
	return 0;
}

// once

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
	STUB();
}

// rwlock

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
	return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
	return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
	return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
	return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
	return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
	return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
	return 0;
}

// named semaphores

sem_t *sem_open(const char *name, int oflag, ...) {
	STUB();
}
