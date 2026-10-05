#include <stdio.h>
#include <pthread.h>

#define NUM_THREADS 4
#define ITERATIONS 1000000

long counter = 0;

void *increment(void *arg) {
    for (int i = 0; i < ITERATIONS; i++) {
        counter++;
    }

    return NULL;
}

int main() {
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, increment, NULL);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Expected counter: %ld\n",
           (long)NUM_THREADS * ITERATIONS);

    printf("Actual counter: %ld\n", counter);

    return 0;
}
