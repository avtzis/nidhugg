// nidhuggc: -tso -optimal
#include <pthread.h>

volatile int x = 0;
volatile int y = 0;

void* thr1(void* param) {
  x = 1;
  int r1 = x;
  y = 1;
  r1 = y;

  pthread_exit(NULL);
}

void* thr2(void* param) {
  x = 2;
  y = 2;
  int r2 = x;

  pthread_exit(NULL);
}

void* thr3(void* param) {
  x = 3;
  y = 3;
  int r3 = x;

  pthread_exit(NULL);
}

int main() {
  pthread_t t1, t2, t3;
  pthread_create(&t1, NULL, thr1, NULL);
  pthread_create(&t2, NULL, thr2, NULL);
  pthread_create(&t3, NULL, thr3, NULL);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  pthread_join(t3, NULL);

  return 0;
}
