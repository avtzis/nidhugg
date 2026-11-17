// nidhuggc: -tso -observers
#include <pthread.h>

volatile int x = 0;

void* thr1(void* param) {
  x = 1;
  x = 11;
  int r1 = x;

  pthread_exit(NULL);
}

void* thr2(void* param) {
  x = 2;
  x = 22;
  int r2 = x;

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
