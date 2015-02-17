
#include <stdlib.h>
#include <stdio.h>
#include "../src/dct.h"
#include "../src/filter.h"

#define MAXN 32

void usage(char *str) {
  fprintf(stderr, "usage: %s <log size - 2> <coeff | mag>\n", str);
}

int main(int argc, char **argv) {
  int i;
  int j;
  int n;
  int n2;
  int ln;
  int left;
  int right;
  int magnitude;
  od_coeff x0[2*MAXN];
  od_coeff y0[2*MAXN];
  od_coeff *x;
  od_coeff *y;
  if (argc != 3) {
    usage(argv[0]);
    return 1;
  }
  ln = atoi(argv[1]);
  if (strcmp("mag", argv[2]) == 0) {
    magnitude = 1;
  }
  else if (strcmp("coeff", argv[2]) == 0) {
    magnitude = 0;
  } else {
    usage(argv[0]);
    return 1;
  }
  n = 4 << ln;
  n2 = n/2;
  left = OD_FILT_SIZE[OD_MINI(OD_NBSIZES - 1, ln + 1)];
  right = OD_FILT_SIZE[ln];
  for (i = 0; i < n; i++) {
    OD_CLEAR(x0, 2*MAXN);
    OD_CLEAR(y0, 2*MAXN);
    x = &x0[n2];
    y = &y0[n2];
    x[i] = 1024;
    OD_IDCT_1D[ln](y, 1, x);
    OD_POST_FILTER[left](y - (2 << left), y - (2 << left));
    OD_POST_FILTER[right](y + n - (2 << right), y + n - (2 << right));
    if (magnitude) {
      double sum;
      sum = 0;
      for (j = 0; j < n + (2 << left) + (2 << right); j++) {
        sum += y[j - (2 << left)]*y[j - (2 << left)];
      }
      printf("%f ", sqrt(sum)/(1<<10));
    }
    else {
      for (j = 0; j < n + (2 << left) + (2 << right); j++) {
        printf("%d ", y[j - (2 << left)]);
      }
      printf("\n");
    }
  }
  if (magnitude) printf("\n");
  return 0;
}
