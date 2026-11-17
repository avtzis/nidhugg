// nidhuggc: -tso -optimal -assume-await
#include <pthread.h>

volatile int x = 0;

void* thr1(void* param) {
  x = 1;
  x = 0;

  pthread_exit(NULL);
}

void* thr2(void* param) {
  x = 0;
  x = 1;

  pthread_exit(NULL);
}

void* thr3(void* param) {
  while (x == 0); // await(x != 0)

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
