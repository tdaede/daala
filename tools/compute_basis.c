
#include <stdlib.h>
#include <stdio.h>
#include "../src/dct.h"
#include "../src/filter.h"

#define MAXN 32

void usage(char *str) {
  fprintf(stderr, "usage: %s <log size - 2> <coeff | mag>\n", str);
}

#define SIZE (3*MAXN)
#define SIZE2 (SIZE*SIZE)

int main(int argc, char **argv) {
  int i;
  int j;
  int n;
  int ln;
  int left;
  int right;
  int magnitude;
  od_coeff x0[SIZE];
  od_coeff y0[SIZE];
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
  left = OD_FILT_SIZE[OD_MINI(OD_NBSIZES - 1, ln + 1)];
  right = OD_FILT_SIZE[ln];
  if (0) {
  for (i = 0; i < n; i++) {
    OD_CLEAR(x0, SIZE);
    OD_CLEAR(y0, SIZE);
    x = &x0[n];
    y = &y0[n];
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
  }
  else {
    od_coeff x0[SIZE2];
    od_coeff y0[SIZE2];
    od_coeff *x;
    od_coeff *y;
    int stride;
    stride = SIZE;
    for (i = 0; i < n; i++) {
      int j;
      for (j = 0; j < n; j++) {
        int k;
        OD_CLEAR(x0, SIZE2);
        OD_CLEAR(y0, SIZE2);
        x = &x0[n*stride + n];
        y = &y0[n*stride + n];
        x[i*stride + j] = 16384;
        OD_IDCT_2D_C[ln](y, stride, x, stride);
        for (k = 0; k < 3*n; k++) {
          od_coeff *y1;
          y1 = y + (k - n)*stride;
          OD_POST_FILTER[left](y1 - (2 << left), y1 - (2 << left));
          OD_POST_FILTER[right](y1 + n - (2 << right), y1 + n - (2 << right));
        }
        for (k = 0; k < 3*n; k++) {
          od_coeff *y1;
          od_coeff tmp[MAXN];
          int m;
          y1 = y + (k - n);
          for (m = 0; m < 4 << left; m++) tmp[m] = y1[(m - (2 << left))*stride];
          OD_POST_FILTER[left](tmp, tmp);
          for (m = 0; m < 4 << left; m++) y1[(m - (2 << left))*stride] = tmp[m];
          for (m = 0; m < 4 << right; m++) tmp[m] = y1[(m + n - (2 << right))*stride];
          OD_POST_FILTER[right](tmp, tmp);
          for (m = 0; m < 4 << right; m++) y1[(m + n - (2 << right))*stride] = tmp[m];
        }
        if (magnitude) {
          int m;
          int k;
          double sum;
          sum = 0;
          for (k = 0; k < 3*n; k++) {
            for (m = 0; m < 3*n; m++) {
              sum += y0[k*stride + m]*y0[k*stride + m];
            }
          }
          printf("%f ", sqrt(sum)/16384);
        }
        if (0 && i == 0 && j == 0) {
          int m;
          int k;
          for (k = 0; k < 3*n; k++) {
            for (m = 0; m < 3*n; m++) {
              printf("%d ", y0[k*stride + m]);
            }
            printf("\n");
          }
          return 1;
        }
      }
      if (magnitude) printf("\n");
    }
  }
  return 0;
}
