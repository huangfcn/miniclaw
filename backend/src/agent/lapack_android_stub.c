#include <stdio.h>
#include <stdlib.h>

// These symbols are missing when building OpenBLAS with NOFORTRAN=1 and C_LAPACK=1 on Android.
// They are referenced by faiss or OpenBLAS internal LAPACK routines.
// We provide stubs here to satisfy the linker.

void sgeqrf_(int *m, int *n, float *a, int *lda, float *tau, float *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: sgeqrf_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void sorgqr_(int *m, int *n, int *k, float *a, int *lda, float *tau, float *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: sorgqr_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void strtri_(char *uplo, char *diag, int *n, float *a, int *lda, int *info) {
    fprintf(stderr, "ERROR: strtri_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void dtrtri_(char *uplo, char *diag, int *n, double *a, int *lda, int *info) {
    fprintf(stderr, "ERROR: dtrtri_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void sgetri_(int *n, float *a, int *lda, int *ipiv, float *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: sgetri_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void dgetri_(int *n, double *a, int *lda, int *ipiv, double *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: dgetri_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void sgetrf_(int *m, int *n, float *a, int *lda, int *ipiv, int *info) {
    fprintf(stderr, "ERROR: sgetrf_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void dgetrf_(int *m, int *n, double *a, int *lda, int *ipiv, int *info) {
    fprintf(stderr, "ERROR: dgetrf_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void sgelsd_(int *m, int *n, int *nrhs, float *a, int *lda, float *b, int *ldb, float *s, float *rcond, int *rank, float *work, int *lwork, int *iwork, int *info) {
    fprintf(stderr, "ERROR: sgelsd_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void dgelsd_(int *m, int *n, int *nrhs, double *a, int *lda, double *b, int *ldb, double *s, double *rcond, int *rank, double *work, int *lwork, int *iwork, int *info) {
    fprintf(stderr, "ERROR: dgelsd_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void ssyev_(char *jobz, char *uplo, int *n, float *a, int *lda, float *w, float *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: ssyev_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void dsyev_(char *jobz, char *uplo, int *n, double *a, int *lda, double *w, double *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: dsyev_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void sgesvd_(char *jobu, char *jobvt, int *m, int *n, float *a, int *lda, float *s, float *u, int *ldu, float *vt, int *ldvt, float *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: sgesvd_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}

void dgesvd_(char *jobu, char *jobvt, int *m, int *n, double *a, int *lda, double *s, double *u, int *ldu, double *vt, int *ldvt, double *work, int *lwork, int *info) {
    fprintf(stderr, "ERROR: dgesvd_ called but not implemented for Android stub!\n");
    if (info) *info = -999;
}
