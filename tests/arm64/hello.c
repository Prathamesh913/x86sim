// Stage 0 test (a): printf through the libsystem thunk boundary (variadic args are on the stack on Apple arm64).
#include <stdio.h>
int main(int argc, char **argv) {
    printf("hello from arm64: %s %d %ld %.3f argc=%d\n", "x86sim", 42, -1234567890123L, 3.25, argc);
    puts(argv[0]);
    return 7;
}
