// nidhuggc: -tso -optimal -assume-await
#include <pthread.h>
#include <stdint.h>

volatile int x = 0;

void* thr1(void* param) {
  char *byte_ptr = (char *)&x;
  __atomic_fetch_add(&byte_ptr[1], 1, __ATOMIC_SEQ_CST);
  __atomic_fetch_add(&byte_ptr[1], 2, __ATOMIC_SEQ_CST);

  pthread_exit(NULL);
}

void* thr2(void* param) {
  char *byte_ptr = (char *)&x;
  byte_ptr[0] = 5;
  while (x != 256);

  pthread_exit(NULL);
}

int main() {
  pthread_t t1, t2;
  pthread_create(&t1, NULL, thr1, NULL);
  pthread_create(&t2, NULL, thr2, NULL);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  return 0;
}
